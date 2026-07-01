/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * USB CDC-ACM interface (the radio).
 *
 * Opens a single CDC-ACM device by VID/PID (default Tyt MD-9600 0x1FC9:0x0094),
 * applies line coding (default 115200 8N1) and transparently bridges it to the
 * message router as MSG_IF_USB_CDC. The component self-registers its router
 * sink, so after usb_cdc_init() frames flow both ways:
 *   router  -> cdc_acm_host_data_tx_blocking()   (sink, TX to the radio)
 *   CDC RX  -> msg_submit(MSG_IF_USB_CDC)        (radio -> router)
 *
 * Hot-plug tolerant: on a sudden USB disconnect the device is closed and the
 * background task re-opens it automatically.
 *
 * Configuration: menuconfig -> Component config -> USB CDC (VID / PID / baud /
 * TX timeout).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the USB Host library and CDC-ACM driver, register the USB CDC
 * router sink, and launch the background tasks that open the configured
 * CDC-ACM device and reconnect after disconnects. Returns immediately; the
 * device is opened in the background.
 *
 * Must be called after msg_router_init() and before msg_router_start().
 */
esp_err_t usb_cdc_init(void);

#ifdef __cplusplus
}
#endif
