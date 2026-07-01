/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART Radio interface.
 *
 * A serial AT-command device whose baud rate is unknown at boot. The driver
 * probes 9600 and 115200 by sending "AT" and listening for an "OK" reply, and
 * keeps retrying (so hot-plug works). Once a valid baud is found the RX task
 * is started and the interface registers itself as a router sink.
 *
 * Configuration: UART peripheral + TX/RX GPIOs are set in menuconfig
 * (Component config -> UART Bridge).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the UART Radio driver, register its router sink and launch the
 * auto-baud + RX tasks. Returns immediately; baud detection happens in the
 * background.
 */
esp_err_t uart_radio_init(void);

#ifdef __cplusplus
}
#endif
