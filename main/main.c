/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: CC0-1.0
 *
 * Tyt MD-9600 multi-interface bridge.
 *
 * Bridges four transparent byte channels through a central message router
 * (components/msg_router), with per-source routing policy implemented as a
 * switch in msg_router.c:
 *
 *   - USB CDC-ACM   : the radio (Tyt MD-9600, 0x1FC9:0x0094)
 *   - BLE Nordic UART Service (multi-connection, up to CONFIG_BT_NIMBLE_MAX_CONNECTIONS)
 *   - UART Radio    : AT device, auto-baud 9600/115200
 *   - UART Lora     : fixed baud (configurable)
 *
 * By default every frame is forwarded to ALL other interfaces. Edit
 * route_message() in msg_router.c to set per-source forwarding rules.
 *
 * app_main only wires the pieces together: every interface component
 * (usb_cdc, ble_bridge, uart_bridge) self-registers its router sink inside its
 * *_init(), so this file just inits the router, then each interface, then
 * starts routing. All I/O, blocking calls and per-interface tasks live in the
 * components.
 */

#include "esp_log.h"
#include "esp_err.h"

#include "msg_router.h"
#include "usb_cdc.h"
#include "ble_bridge.h"
#include "uart_radio.h"
#include "uart_lora.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    /* --- Message router: create queue, register sinks, start task -------- */
    ESP_ERROR_CHECK(msg_router_init());

    /* Each interface component self-registers its router sink. */
    ESP_ERROR_CHECK(usb_cdc_init());
    ESP_ERROR_CHECK(uart_radio_init());
    ESP_ERROR_CHECK(uart_lora_init());
    ESP_ERROR_CHECK(ble_bridge_init());

    ESP_ERROR_CHECK(msg_router_start());

    ESP_LOGI(TAG, "Bridge up: USB CDC + BLE + UART Radio + UART Lora");
}
