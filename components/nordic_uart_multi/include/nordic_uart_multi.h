/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Multi-connection Nordic UART Service (NUS) over NimBLE.
 *
 * Implements the standard Nordic UART Service
 *   6E400001-B5A3-F393-E0A9-E50E24DCCA9E  (service)
 *   6E400002-...  RX  (peer -> us, write)
 *   6E400003-...  TX  (us -> peer, notify)
 *
 * Supports up to CONFIG_BT_NIMBLE_MAX_CONNECTIONS simultaneous peers.
 *  - Data written by any peer goes into a single RX ring buffer (broadcast-in).
 *  - nordic_uart_send()        broadcasts to every subscribed peer.
 *  - nordic_uart_send_to()     sends to a single peer.
 *  - nordic_uart_send_except() sends to every peer except one.
 *
 * Based on the Espressif ble_spp/spp_server example pattern, with NUS UUIDs.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One received frame stored in the RX ring buffer (RINGBUF_TYPE_NOSPLIT).
 * The actual payload bytes follow the header (flexible array member).
 *
 * Use xRingbufferReceive() to obtain a const pointer to this struct,
 * read item->data / item->len, then vRingbufferReturnItem().
 */
typedef struct {
    uint16_t conn_handle; /**< Source connection handle            */
    uint16_t len;         /**< Number of valid bytes in data[]     */
    uint8_t  data[];      /**< Raw payload (binary, transparent)   */
} nordic_uart_rx_item_t;

/** RX ring buffer handle. NULL until nordic_uart_start() succeeds. */
extern RingbufHandle_t nordic_uart_rx_buf_handle;

/**
 * Start the multi-connection Nordic UART peripheral.
 *
 * @param device_name Advertised BLE device name (NULL -> "Nordic UART").
 * @return ESP_OK on success.
 */
esp_err_t nordic_uart_start(const char *device_name);

/** Stop the service and free resources. */
esp_err_t nordic_uart_stop(void);

/**
 * Send raw bytes to ALL connected and subscribed peers (broadcast).
 * Data is chunked per-peer according to its negotiated ATT MTU.
 * Transparent / binary safe.
 *
 * @return ESP_OK (individual peer errors are logged, not fatal).
 */
esp_err_t nordic_uart_send(const uint8_t *data, size_t len);

/** Convenience: send a NUL-terminated string to all peers. */
esp_err_t nordic_uart_send_str(const char *str);

/**
 * Send raw bytes to a single peer.
 *
 * @return ESP_OK on success,
 *         ESP_ERR_NOT_FOUND     if conn_handle is not connected,
 *         ESP_ERR_INVALID_STATE if the peer is connected but not subscribed
 *                               to TX notifications.
 */
esp_err_t nordic_uart_send_to(uint16_t conn_handle, const uint8_t *data, size_t len);

/** Convenience: send a NUL-terminated string to a single peer. */
esp_err_t nordic_uart_send_str_to(uint16_t conn_handle, const char *str);

/**
 * Send raw bytes to every subscribed peer EXCEPT the given conn_handle.
 * If conn_handle is out of range or not connected, this is equivalent to
 * nordic_uart_send() (i.e. broadcast to all).
 */
esp_err_t nordic_uart_send_except(uint16_t conn_handle, const uint8_t *data, size_t len);

/** Convenience: send a NUL-terminated string to all peers except one. */
esp_err_t nordic_uart_send_str_except(uint16_t conn_handle, const char *str);

/** Number of currently connected peers. */
uint8_t nordic_uart_client_count(void);

/** Number of peers currently subscribed to TX notifications. */
uint8_t nordic_uart_subscribed_count(void);

#ifdef __cplusplus
}
#endif
