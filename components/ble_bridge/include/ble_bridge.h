/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BLE bridge to the message router (Nordic UART Service).
 *
 * Thin glue over the nordic_uart_multi peripheral driver: it starts the NUS
 * peripheral (advertising as CONFIG_BLE_DEVICE_NAME), self-registers the BLE
 * router sink and launches the BLE -> router drain task, so after
 * ble_bridge_init() frames flow both ways:
 *   router -> nordic_uart_send[_except]()   (sink, TX to BLE peers)
 *   BLE RX -> msg_submit_from(MSG_IF_BLE)   (peer -> router, tagged with the
 *                                           sender conn_handle so the sink can
 *                                           exclude it)
 *
 * Keeping this glue out of nordic_uart_multi lets the peripheral driver stay a
 * router-free, reusable NUS implementation (mirroring how usb_cdc / uart_bridge
 * wrap their respective drivers).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the BLE Nordic UART peripheral (advertising as CONFIG_BLE_DEVICE_NAME),
 * register the BLE router sink, and launch the BLE -> router drain task.
 *
 * Must be called after msg_router_init() and before msg_router_start().
 * Returns immediately; BLE connections are accepted in the background.
 */
esp_err_t ble_bridge_init(void);

#ifdef __cplusplus
}
#endif
