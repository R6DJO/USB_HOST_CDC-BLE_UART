/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART Lora interface.
 *
 * A fixed-baud serial link (default 9600). Baud rate and TX/RX GPIOs are set
 * in menuconfig (Component config -> UART Bridge). The interface registers
 * itself as a router sink and forwards received bytes into the router.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the UART Lora driver, register its router sink and launch the RX
 * task.
 */
esp_err_t uart_lora_init(void);

#ifdef __cplusplus
}
#endif
