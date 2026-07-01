/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Central message router.
 *
 * Every interface in the system (USB CDC, BLE, UART Radio, UART Lora, ...)
 * is both a *source* and a *destination* of data. Producers push received
 * frames into the router with msg_submit(); a single msg_routing task drains
 * the queue and, based on the source, forwards the frame to one or several
 * destinations through per-destination "sink" callbacks registered at startup.
 *
 * Routing policy lives in route_message() in msg_router.c as a switch over the
 * source id, so per-source forwarding rules are trivial to customize.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Logical interface identifier. Used both as a source tag (who produced the
 * frame) and as a destination id (whom to forward to). Add new entries before
 * MSG_IF_COUNT.
 */
typedef enum {
    MSG_IF_USB_CDC    = 0, /**< USB CDC-ACM (the radio / MD-9600)  */
    MSG_IF_BLE        = 1, /**< BLE Nordic UART Service             */
    MSG_IF_UART_RADIO = 2, /**< UART Radio (AT, auto-baud)         */
    MSG_IF_UART_LORA  = 3, /**< UART Lora (fixed baud)             */
    MSG_IF_COUNT,
} msg_iface_t;

/** Sink callback: deliver one frame to a destination interface. */
typedef void (*msg_sink_fn_t)(const uint8_t *data, size_t len);

/**
 * Create the routing ring buffer and reset the sink registry.
 * Must be called once before any msg_submit() / msg_router_start().
 */
esp_err_t msg_router_init(void);

/**
 * Register (or replace) the sink callback for a destination interface.
 * A destination with no registered sink is simply skipped while routing.
 *
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t msg_router_register_sink(msg_iface_t dest, msg_sink_fn_t fn);

/**
 * Push one received frame into the router, tagged with its source.
 * Safe to call from any task (and from interrupt-style callbacks that are
 * actually invoked from a task context, e.g. the USB CDC data_cb). The frame
 * contents are copied, so the caller may free/reuse @p data immediately.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM if the router buffer
 *         is full (the frame is dropped with a warning in that case).
 */
esp_err_t msg_submit(msg_iface_t src, const uint8_t *data, size_t len);

/**
 * Deliver one frame to a single destination (no-op if that sink is not
 * registered). Intended for use inside the per-source routing switch when you
 * want to send to an explicit set of destinations instead of "all".
 */
void msg_router_send_to(msg_iface_t dest, const uint8_t *data, size_t len);

/**
 * Create the msg_routing task. All sinks should be registered beforehand
 * (sinks may still be registered later / changed at runtime).
 *
 * @return ESP_OK or ESP_FAIL if the task could not be created.
 */
esp_err_t msg_router_start(void);

/** The routing task entry point (exposed for unit testing / custom creation). */
void msg_routing_task(void *arg);

#ifdef __cplusplus
}
#endif
