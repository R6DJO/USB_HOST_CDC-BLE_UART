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
#define NUS_ENOMEM_RETRY_MS 50      /* delay between mbuf-alloc retries, ms   */
#define NUS_MAX_RETRIES     10      /* give-up threshold for a single chunk   */

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

/* nus_peers[] and nus_conn_count are touched from TWO tasks concurrently:
 *   - the NimBLE host task (nus_gap_event: connect/disconnect/subscribe/mtu),
 *   - the message-routing task (nus_send_common -> nus_peer_send -> notify).
 * Guard every access with a spinlock. Critical sections are tiny and contain
 * NO blocking calls (no notify, no advertise, no logging inside the lock). */
static portMUX_TYPE nus_lock = portMUX_INITIALIZER_UNLOCKED;

/* A peer selected for a notify, snapshotted under nus_lock so the slow
 * ble_gatts_notify_custom() call runs lock-free. */
typedef struct {
    uint16_t h;
    uint16_t mtu;
} nus_target_t;

/* ------------------------------------------------------------------ helpers */

/* True if conn_handle indexes a valid nus_peers[] slot. The table is sized
 * MAX+1 (see NUS_PEER_ARRAY_SIZE), so any real NimBLE handle fits. */
static inline bool nus_handle_ok(uint16_t h)
{
    return h < NUS_PEER_ARRAY_SIZE;
}

/* --- locked accessors (each briefly takes nus_lock; non-blocking) -------- */

static uint8_t nus_conn_count_get(void)
{
    uint8_t c;
    portENTER_CRITICAL(&nus_lock);
    c = nus_conn_count;
    portEXIT_CRITICAL(&nus_lock);
    return c;
}

/* Mark a peer freshly connected and bump the connection count. */
static void nus_peer_connect(uint16_t h)
{
    portENTER_CRITICAL(&nus_lock);
    memset(&nus_peers[h], 0, sizeof(nus_peer_t));
    nus_peers[h].connected = true;
    nus_conn_count++;
    portEXIT_CRITICAL(&nus_lock);
}

/* Clear a peer slot if it is still connected, fixing the connection count.
 * Returns true if the slot actually transitioned connected -> free.
 * Idempotent: safe to call after the BLE_GAP_EVENT_DISCONNECT has run. */
static bool nus_peer_disconnect(uint16_t h)
{
    bool changed = false;
    portENTER_CRITICAL(&nus_lock);
    if (nus_handle_ok(h) && nus_peers[h].connected) {
        memset(&nus_peers[h], 0, sizeof(nus_peer_t));
        if (nus_conn_count > 0) {
            nus_conn_count--;
        }
        changed = true;
    }
    portEXIT_CRITICAL(&nus_lock);
    return changed;
}

static void nus_peer_set_subscribed(uint16_t h, bool v)
{
    portENTER_CRITICAL(&nus_lock);
    nus_peers[h].subscribed = v;
    portEXIT_CRITICAL(&nus_lock);
}

static void nus_peer_set_mtu(uint16_t h, uint16_t v)
{
    portENTER_CRITICAL(&nus_lock);
    nus_peers[h].mtu = v;
    portEXIT_CRITICAL(&nus_lock);
}

/* ----------------------------------------------------------- GAP advertising */

static int nus_gap_event(struct ble_gap_event *event, void *arg);

static void nus_advertise(void)
{
    if (nus_conn_count_get() >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
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

    /* Advertising payload carries only flags + tx power + the 128-bit service
     * UUID (what NUS clients filter on). The full device name is published in
     * the scan response below — no truncated short name in the primary packet. */

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

/** Central GAP event dispatcher (connect / disconnect / subscribe / MTU /
 *  advertising-complete). Runs in the NimBLE host task context, so every
 *  branch keeps the per-peer state mutations under nus_lock and never blocks. */
static int nus_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t h = event->connect.conn_handle;
            if (nus_handle_ok(h)) {
                nus_peer_connect(h);
            }
            uint8_t total = nus_conn_count_get();
            ESP_LOGI(TAG, "Connected handle=%u  total=%u/%u",
                     h, total, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
            if (total < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
                nus_advertise(); /* keep accepting more peers */
            }
        } else {
            ESP_LOGW(TAG, "Connect failed status=%d", event->connect.status);
            nus_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        nus_peer_disconnect(h);            /* idempotent: clears slot + count */
        uint8_t total = nus_conn_count_get();
        ESP_LOGI(TAG, "Disconnected handle=%u reason=%d  total=%u/%u",
                 h, event->disconnect.reason,
                 total, CONFIG_BT_NIMBLE_MAX_CONNECTIONS);
        nus_advertise(); /* a slot freed up */
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t h = event->subscribe.conn_handle;
        if (nus_handle_ok(h) &&
            event->subscribe.attr_handle == nus_tx_val_handle) {
            nus_peer_set_subscribed(h, event->subscribe.cur_notify);
            ESP_LOGI(TAG, "Subscribe handle=%u notify=%d", h,
                     event->subscribe.cur_notify);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU: {
        uint16_t h = event->mtu.conn_handle;
        if (nus_handle_ok(h)) {
            nus_peer_set_mtu(h, event->mtu.value);
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

/** GATT access callback for the NUS RX/TX characteristics. Only writes to the
 *  RX char (peer -> us) carry data: each write is framed with the sender's
 *  conn_handle and pushed into the RX ring buffer. Reads of the TX char are
 *  no-ops (TX is notify-only). */
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
        ESP_LOGI(TAG, "RX write conn=%u len=%u", conn_handle, len);
        if (len == 0) {
            return 0;
        }

        /* Zero-copy: acquire storage inside the RX ring buffer and copy the
         * mbuf straight into it (no malloc / memcpy / free per write). */
        size_t item_size = sizeof(nordic_uart_rx_item_t) + len;
        nordic_uart_rx_item_t *item;
        if (xRingbufferSendAcquire(nordic_uart_rx_buf_handle, (void **)&item,
                                   item_size,
                                   pdMS_TO_TICKS(NUS_NOTIFY_TIMEOUT)) != pdTRUE) {
            ESP_LOGW(TAG, "RX ring buffer full, dropping %u bytes from handle %u",
                     len, conn_handle);
            return 0;
        }
        item->conn_handle = conn_handle;
        item->len = len;
        if (os_mbuf_copydata(ctxt->om, 0, len, item->data) != 0) {
            /* Should not happen (len == PKTLEN). Publish an empty item so the
             * consumer drops it, and never leave an acquired slot uncompleted
             * (that would stall the whole ring buffer). */
            ESP_LOGE(TAG, "RX mbuf copydata failed len=%u", len);
            item->len = 0;
        }
        xRingbufferSendComplete(nordic_uart_rx_buf_handle, item);
        return 0;
    }

    return 0;
}

/* Nordic UART Service GATT table: one primary service with two characteristics
 * -- RX (peer writes data to us) and TX (we notify peers). The TX char's
 * value handle is captured into nus_tx_val_handle so the GAP subscribe event
 * can tell TX subscriptions apart from any other characteristic. */
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

/* Send one frame to a single peer, chunked by ATT MTU. The whole frame is
 * delivered ATOMICALLY: every chunk's mbuf is allocated first, and only then
 * do we start notifying. If any allocation fails, the already-allocated mbufs
 * are freed and NOTHING is sent -- so a peer never receives a partial frame
 * (which would silently corrupt a binary-transparent stream).
 *
 * ble_gatts_notify_custom() takes ownership of the mbuf on every return path
 * (the BLE stack frees it), matching the original streaming implementation,
 * so we never free an mbuf we already handed to it.
 * Returns 0 on success, or a BLE_HS_* error code. */
static int nus_peer_send(uint16_t h, uint16_t mtu, const uint8_t *data, size_t len)
{
    size_t chunk = (mtu ? mtu : NUS_DEFAULT_MTU) - 3; /* 3 bytes ATT header */
    if (chunk == 0) {
        chunk = 1;
    }
    int nchunks = (int)((len + chunk - 1) / chunk);

    struct os_mbuf **mbs = calloc((size_t)nchunks, sizeof(*mbs));
    if (mbs == NULL) {
        ESP_LOGW(TAG, "handle=%u: mbuf-array alloc failed (%d chunks)", h, nchunks);
        return BLE_HS_ENOMEM;
    }

    /* Phase 1: allocate every chunk's mbuf (with the usual ENOMEM retry). */
    int rc = 0;
    int got = 0;
    for (int c = 0; c < nchunks; c++) {
        size_t off = (size_t)c * chunk;
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        struct os_mbuf *om = NULL;
        for (int retry = 0; retry < NUS_MAX_RETRIES; retry++) {
            om = ble_hs_mbuf_from_flat(data + off, n);
            if (om != NULL) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(NUS_ENOMEM_RETRY_MS));
        }
        if (om == NULL) {
            rc = BLE_HS_ENOMEM;
            break;
        }
        mbs[got++] = om;
    }

    /* Phase 2: all mbufs ready -> notify each (notify_custom consumes the mbuf
     * on every return path). Stop on the first hard error. */
    if (rc == 0) {
        for (int c = 0; c < nchunks; c++) {
            int r = ble_gatts_notify_custom(h, nus_tx_val_handle, mbs[c]);
            mbs[c] = NULL;   /* consumed by the BLE stack (success or failure) */
            if (r == 0) {
                continue;
            }
            if (r == BLE_HS_ENOTCONN) {
                ESP_LOGW(TAG, "notify handle=%u gone, dropping peer", h);
                nus_peer_disconnect(h);  /* idempotent clear + count fix */
            } else {
                ESP_LOGW(TAG, "notify handle=%u rc=%d", h, r);
            }
            rc = r;
            break;
        }
    } else {
        ESP_LOGW(TAG, "handle=%u: mbuf alloc exhausted, dropping %u-byte frame "
                 "(not partially sent)", h, (unsigned)len);
    }

    /* Free any mbuf we allocated but did NOT hand to notify_custom (allocation
     * abort, or a notify error that stopped us mid-frame). NULL entries were
     * already consumed by the stack. */
    for (int c = 0; c < got; c++) {
        if (mbs[c] != NULL) {
            os_mbuf_free_chain(mbs[c]);
        }
    }
    free(mbs);
    return rc;
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
        bool connected, subscribed;
        portENTER_CRITICAL(&nus_lock);
        connected  = nus_handle_ok(target) && nus_peers[target].connected;
        subscribed = connected && nus_peers[target].subscribed;
        portEXIT_CRITICAL(&nus_lock);
        if (!connected) {
            ESP_LOGW(TAG, "send_to handle=%u not connected", target);
            return ESP_ERR_NOT_FOUND;
        }
        if (!subscribed) {
            ESP_LOGW(TAG, "send_to handle=%u not subscribed", target);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* Snapshot the target set under the lock, then notify lock-free: the
     * notify call (and its retry loop) is slow and must not hold nus_lock. */
    nus_target_t targets[NUS_PEER_ARRAY_SIZE];
    size_t n = 0;
    portENTER_CRITICAL(&nus_lock);
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
        targets[n].h   = h;
        targets[n].mtu = nus_peers[h].mtu ? nus_peers[h].mtu : NUS_DEFAULT_MTU;
        n++;
    }
    portEXIT_CRITICAL(&nus_lock);

    for (size_t i = 0; i < n; i++) {
        nus_peer_send(targets[i].h, targets[i].mtu, data, len);
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
    return nus_conn_count_get();
}

uint8_t nordic_uart_subscribed_count(void)
{
    uint8_t c = 0;
    portENTER_CRITICAL(&nus_lock);
    for (uint16_t h = 0; h < NUS_PEER_ARRAY_SIZE; h++) {
        if (nus_peers[h].connected && nus_peers[h].subscribed) {
            c++;
        }
    }
    portEXIT_CRITICAL(&nus_lock);
    return c;
}

/* --------------------------------------------------------------- lifecycle */

/** BLE-host "synced with controller" callback: the controller is ready, so
 *  resolve our preferred address type and begin advertising. Runs once after
 *  nimble_port_init(). */
static void nus_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &nus_own_addr_type);
    assert(rc == 0);
    nus_advertise();
}

/** NimBLE host task. nimble_port_run() blocks until nimble_port_stop() is
 *  called (from nordic_uart_stop()), after which we tear down the FreeRTOS
 *  port glue. */
static void nus_host_task(void *param)
{
    (void)param;
    nimble_port_run();
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

    /* Order matters: ble_svc_gap_init() must run BEFORE
     * ble_svc_gap_device_name_set(). With CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=y
     * (the ESP-IDF default), ble_svc_gap_init() -> ble_svc_gap_init_name()
     * re-initializes the GAP device name to MYNEWT_VAL(BLE_SVC_GAP_DEVICE_NAME)
     * ("nimble"), discarding -- and leaking -- any name set earlier. Calling
     * set() after init() reliably overrides that default with ours. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(device_name);

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
    portENTER_CRITICAL(&nus_lock);
    nus_conn_count = 0;
    memset(nus_peers, 0, sizeof nus_peers);
    portEXIT_CRITICAL(&nus_lock);
    return ESP_OK;
}
