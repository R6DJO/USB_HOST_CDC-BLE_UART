/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BLE bridge to the message router.
 *
 * Owns the two halves of the BLE <-> router path that used to live in main.c:
 *   - ble_sink         : router -> BLE (fan-out notify, excluding the sender)
 *   - ble_to_router_task: NUS RX ring buffer -> msg_submit_from(MSG_IF_BLE)
 * ble_bridge_init() also starts the NUS peripheral itself.
 */
#include "ble_bridge.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include "nordic_uart_multi.h"
#include "msg_router.h"

static const char *TAG = "BLE_BRIDGE";

#define BLE2RT_TASK_STACK 2048
#define BLE2RT_TASK_PRIO  1

/* ----------------------------------------------------------- router sink (TX) */

/** Deliver a frame to BLE peers.
 *  - If the frame came from a BLE peer, send to every OTHER peer (the sender
 *    is excluded via nordic_uart_send_except) -- no send-back to sender.
 *  - Otherwise broadcast to all peers (e.g. a radio reply). */
static void ble_sink(const msg_origin_t *origin, const uint8_t *data, size_t len)
{
    esp_err_t err;
    if (origin->iface == MSG_IF_BLE && origin->peer != MSG_PEER_NONE) {
        err = nordic_uart_send_except(origin->peer, data, len);
    } else {
        err = nordic_uart_send(data, len);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BLE send failed: %s", esp_err_to_name(err));
    }
}

/* ----------------------------------------------------------- BLE -> router */

/** Drain the Nordic UART RX ring buffer (data written by any BLE peer) and
 *  submit each frame to the router tagged with MSG_IF_BLE + the sender's
 *  conn_handle (so the BLE sink can exclude the sender). */
static void ble_to_router_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (nordic_uart_rx_buf_handle == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t item_size = 0;
        const nordic_uart_rx_item_t *item =
            (const nordic_uart_rx_item_t *)xRingbufferReceive(
                nordic_uart_rx_buf_handle, &item_size, portMAX_DELAY);
        if (item == NULL) {
            continue;
        }

        ESP_LOGI(TAG, "peer=%u -> %u byte(s)", item->conn_handle, item->len);
        msg_origin_t origin = { .iface = MSG_IF_BLE, .peer = item->conn_handle };
        msg_submit_from(&origin, item->data, item->len);

        vRingbufferReturnItem(nordic_uart_rx_buf_handle, (void *)item);
    }
}

/* ----------------------------------------------------------- public API */

esp_err_t ble_bridge_init(void)
{
    /* Register the sink first so the router has a destination before the
     * peripheral starts accepting peers. */
    esp_err_t ret = msg_router_register_sink(MSG_IF_BLE, ble_sink);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register sink: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nordic_uart_start(CONFIG_BLE_DEVICE_NAME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nordic_uart_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t ok = xTaskCreate(ble_to_router_task, "ble2rt", BLE2RT_TASK_STACK,
                                NULL, BLE2RT_TASK_PRIO, NULL);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create ble2rt task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
