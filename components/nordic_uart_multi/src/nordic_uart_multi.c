/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Multi-connection Nordic UART Service implementation over NimBLE.
 *
 * Pattern (per-peer subscription tracking, fan-out notify, restart advertising
 * after connect) is borrowed from the official ESP-IDF ble_spp/spp_server
 * example; the GATT service uses the Nordic UART Service UUIDs.
 */
#include "nordic_uart_multi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "NORDIC_UART";

/* Nordic UART Service UUIDs.
 * 6E40xxxx-B5A3-F393-E0A9-E50E24DCCA9E
 * BLE_UUID128_INIT takes the 16 bytes in little-endian (reverse) order:
 *   9E CA DC 24 0E E5 A9 E0 93 F3 A3 B5 xx xx 40 6E
 */
static const ble_uuid128_t NUS_SVC_UUID =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID = /* peer -> us (write) */
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID = /* us -> peer (notify) */
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

/* Sized MAX+1 so a conn_handle == MAX can never overrun the array (matches the
 * ble_spp_server example). Real handles are 0..MAX-1. */
#define NUS_PEER_ARRAY_SIZE (CONFIG_BT_NIMBLE_MAX_CONNECTIONS + 1)
#define NUS_DEFAULT_MTU     23      /* minimal ATT MTU -> 20 byte payload  */
#define NUS_NOTIFY_TIMEOUT  20      /* xRingbufferSend wait, ms            */
#define NUS_ENOMEM_RETRY_MS 50
#define NUS_MAX_RETRIES     10

typedef struct {
    bool     connected;
    bool     subscribed;  /* TX CCCD notifications enabled */
    uint16_t mtu;         /* 0 -> fallback to NUS_DEFAULT_MTU */
} nus_peer_t;

static nus_peer_t nus_peers[NUS_PEER_ARRAY_SIZE];
static uint16_t   nus_tx_val_handle;
static uint8_t    nus_own_addr_type;
static uint8_t    nus_conn_count;

RingbufHandle_t nordic_uart_rx_buf_handle;

/* ------------------------------------------------------------------ helpers */

static inline bool nus_handle_ok(uint16_t h)
{
    return h < NUS_PEER_ARRAY_SIZE;
}

static uint16_t nus_peer_mtu(uint16_t h)
{
    return nus_peers[h].mtu ? nus_peers[h].mtu : NUS_DEFAULT_MTU;
}

/* ----------------------------------------------------------- GAP advertising */

static int nus_gap_event(struct ble_gap_event *event, void *arg);

static void nus_advertise(void)
{
    if (nus_conn_count >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        return; /* at capacity: cannot accept more peers */
    }
    if (ble_gap_adv_active()) {
        return; /* already running */
    }

    const char *name = ble_svc_gap_device_name();
    size_t name_len = strlen(name);

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    /* Short name keeps the 31-byte adv payload valid alongside the
     * 16-byte 128-bit service UUID. Full name goes into scan response. */
    char short_name[6]; /* 5 chars + NUL */
    strncpy(short_name, name, sizeof(short_name));
    short_name[sizeof(short_name) - 1] = '\0';
    fields.name = (uint8_t *)short_name;
    fields.name_len = strlen(short_name);
    fields.name_is_complete = (name_len <= sizeof(short_name) - 1) ? 1 : 0;

    fields.uuids128 = &NUS_SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    /* Scan response: complete device name */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.name = (uint8_t *)name;
    rsp.name_len = name_len;
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d (name too long?)", rc);
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(nus_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, nus_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

/* --------------------------------------------------------------- GAP events */

static int nus_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t h = event->connect.conn_handle;
            nus_conn_count++;
            if (nus_handle_ok(h)) {
                memset(&nus_peers[h], 0, sizeof(nus_peer_t));
                nus_peers[h].connected = true;
            }
            ESP_LOGI(TAG, "Connected handle=%u  total=%u/%u",
                     h, nus_conn_count, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
            if (nus_conn_count < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
                nus_advertise(); /* keep accepting more peers */
            }
        } else {
            ESP_LOGW(TAG, "Connect failed status=%d", event->connect.status);
            nus_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        if (nus_handle_ok(h)) {
            memset(&nus_peers[h], 0, sizeof(nus_peer_t));
        }
        if (nus_conn_count > 0) {
            nus_conn_count--;
        }
        ESP_LOGI(TAG, "Disconnected handle=%u reason=%d  total=%u/%u",
                 h, event->disconnect.reason,
                 nus_conn_count, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
        nus_advertise(); /* a slot freed up */
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = event->subscribe.conn_handle;
        if (nus_handle_ok(h) &&
            event->subscribe.attr_handle == nus_tx_val_handle) {
            nus_peers[h].subscribed = event->subscribe.cur_notify ? true : false;
            ESP_LOGI(TAG, "Subscribe handle=%u notify=%d", h,
                     event->subscribe.cur_notify);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        uint16_t h = event->mtu.conn_handle;
        if (nus_handle_ok(h)) {
            nus_peers[h].mtu = event->mtu.value;
        }
        ESP_LOGD(TAG, "MTU handle=%u mtu=%u", h, event->mtu.value);
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        nus_advertise();
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------- GATT service */

static int nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0; /* reads of TX char are no-ops */
    }

    /* RX characteristic: peer wrote data to us -> push framed item to ring buf */
    if (ble_uuid_cmp(ctxt->chr->uuid, &NUS_RX_UUID.u) == 0) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) {
            return 0;
        }

        size_t item_size = sizeof(nordic_uart_rx_item_t) + len;
        nordic_uart_rx_item_t *item = malloc(item_size);
        if (item == NULL) {
            ESP_LOGE(TAG, "RX malloc fail len=%u", len);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        item->conn_handle = conn_handle;
        item->len = len;
        if (os_mbuf_copydata(ctxt->om, 0, len, item->data) != 0) {
            free(item);
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (xRingbufferSend(nordic_uart_rx_buf_handle, item, item_size,
                            pdMS_TO_TICKS(NUS_NOTIFY_TIMEOUT)) != pdTRUE) {
            ESP_LOGW(TAG, "RX ring buffer full, dropping %u bytes from handle %u",
                     len, conn_handle);
        }
        free(item); /* xRingbufferSend copied the contents */
        return 0;
    }

    return 0;
}

static const struct ble_gatt_svc_def nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &NUS_RX_UUID.u,
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &NUS_TX_UUID.u,
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* --------------------------------------------------------------- TX fan-out */

typedef enum {
    NUS_SEND_ALL,        /* broadcast to every subscribed peer            */
    NUS_SEND_ONE,        /* send to a single conn_handle only             */
    NUS_SEND_ALL_EXCEPT, /* send to every subscribed peer except one      */
} nus_send_mode_t;

/* Send one chunked notification to a single peer.
 * Returns 0 on success, or a BLE_HS_* error code. On BLE_HS_ENOTCONN the
 * peer is dropped (it has gone away). */
static int nus_peer_send(uint16_t h, const uint8_t *data, size_t len)
{
    size_t chunk = nus_peer_mtu(h) - 3; /* 3 bytes ATT header overhead */

    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;

        int rc = BLE_HS_EAGAIN;
        for (int retry = 0; retry < NUS_MAX_RETRIES; retry++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, n);
            if (om == NULL) {
                vTaskDelay(pdMS_TO_TICKS(NUS_ENOMEM_RETRY_MS));
                continue;
            }
            rc = ble_gatts_notify_custom(h, nus_tx_val_handle, om);
            if (rc == 0 || rc != BLE_HS_ENOMEM) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(NUS_ENOMEM_RETRY_MS));
        }

        if (rc == BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "notify handle=%u gone, dropping peer", h);
            memset(&nus_peers[h], 0, sizeof(nus_peer_t));
            if (nus_conn_count > 0) {
                nus_conn_count--;
            }
            return BLE_HS_ENOTCONN;
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "notify handle=%u rc=%d", h, rc);
            return rc;
        }
    }
    return 0;
}

/* Core send routine with a target selector.
 *  - NUS_SEND_ALL:          target is ignored.
 *  - NUS_SEND_ONE:          only target; returns ESP_ERR_NOT_FOUND if it is
 *                           not connected, ESP_ERR_INVALID_STATE if not
 *                           subscribed.
 *  - NUS_SEND_ALL_EXCEPT:   everyone but target; an out-of-range target means
 *                           "send to all".
 * Individual peer failures are logged, not fatal (return stays ESP_OK) unless
 * the (single) requested peer in NUS_SEND_ONE is missing. */
static esp_err_t nus_send_common(const uint8_t *data, size_t len,
                                 nus_send_mode_t mode, uint16_t target)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mode == NUS_SEND_ONE) {
        if (!nus_handle_ok(target) || !nus_peers[target].connected) {
            ESP_LOGW(TAG, "send_to handle=%u not connected", target);
            return ESP_ERR_NOT_FOUND;
        }
        if (!nus_peers[target].subscribed) {
            ESP_LOGW(TAG, "send_to handle=%u not subscribed", target);
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (uint16_t h = 0; h < NUS_PEER_ARRAY_SIZE; h++) {
        if (!nus_peers[h].connected || !nus_peers[h].subscribed) {
            continue;
        }
        if (mode == NUS_SEND_ONE && h != target) {
            continue;
        }
        if (mode == NUS_SEND_ALL_EXCEPT && h == target) {
            continue;
        }
        nus_peer_send(h, data, len);
    }

    return ESP_OK;
}

esp_err_t nordic_uart_send(const uint8_t *data, size_t len)
{
    return nus_send_common(data, len, NUS_SEND_ALL, 0);
}

esp_err_t nordic_uart_send_str(const char *str)
{
    if (str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nordic_uart_send((const uint8_t *)str, strlen(str));
}

esp_err_t nordic_uart_send_to(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    return nus_send_common(data, len, NUS_SEND_ONE, conn_handle);
}

esp_err_t nordic_uart_send_str_to(uint16_t conn_handle, const char *str)
{
    if (str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nordic_uart_send_to(conn_handle, (const uint8_t *)str, strlen(str));
}

esp_err_t nordic_uart_send_except(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    return nus_send_common(data, len, NUS_SEND_ALL_EXCEPT, conn_handle);
}

esp_err_t nordic_uart_send_str_except(uint16_t conn_handle, const char *str)
{
    if (str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return nordic_uart_send_except(conn_handle, (const uint8_t *)str, strlen(str));
}

uint8_t nordic_uart_client_count(void)
{
    return nus_conn_count;
}

uint8_t nordic_uart_subscribed_count(void)
{
    uint8_t c = 0;
    for (uint16_t h = 0; h < NUS_PEER_ARRAY_SIZE; h++) {
        if (nus_peers[h].connected && nus_peers[h].subscribed) {
            c++;
        }
    }
    return c;
}

/* --------------------------------------------------------------- lifecycle */

static void nus_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &nus_own_addr_type);
    assert(rc == 0);
    nus_advertise();
}

static void nus_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only when nimble_port_stop() is called */
    nimble_port_freertos_deinit();
}

esp_err_t nordic_uart_start(const char *device_name)
{
    if (nordic_uart_rx_buf_handle != NULL) {
        ESP_LOGE(TAG, "Already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* NVS is required by the BLE stack (bonding/keys). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nordic_uart_rx_buf_handle = xRingbufferCreate(CONFIG_NORDIC_UART_RX_BUFFER_SIZE,
                                                  RINGBUF_TYPE_NOSPLIT);
    if (nordic_uart_rx_buf_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create RX ring buffer");
        return ESP_ERR_NO_MEM;
    }

    if (device_name == NULL) {
        device_name = "Nordic UART";
    }

    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_device_name_set(device_name);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = nus_on_sync;

    nimble_port_freertos_init(nus_host_task);
    ESP_LOGI(TAG, "Started (max %u connections)",
             CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
    return ESP_OK;
}

esp_err_t nordic_uart_stop(void)
{
    ble_gap_adv_stop(); /* ignore error if not running */

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }

    if (nordic_uart_rx_buf_handle) {
        vRingbufferDelete(nordic_uart_rx_buf_handle);
        nordic_uart_rx_buf_handle = NULL;
    }
    nus_conn_count = 0;
    memset(nus_peers, 0, sizeof nus_peers);
    return ESP_OK;
}
