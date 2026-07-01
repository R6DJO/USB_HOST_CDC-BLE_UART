/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Central message router.
 *
 * Every interface in the system (USB CDC, BLE, UART Radio, UART Lora, ...)
 * is both a *source* and a *destination* of data. Producers push received
 * frames into the router with msg_submit() / msg_submit_from(); a single
 * msg_routing task drains the queue and forwards each frame to every
 * registered sink.
 *
 * Self-exclusion: a sink must NOT echo a frame back to its own originator.
 *  - Single-port interfaces (USB, UART Radio, UART Lora) just return when the
 *    source interface is their own.
 *  - BLE is multi-peer, so its sink forwards to every peer EXCEPT the
 *    originating one (tracked via msg_origin_t.peer, which carries the BLE
 *    conn_handle). This is what guarantees "no send-back to the sender".
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

/** Sentinel meaning "no specific peer" (used by single-port sources). */
#define MSG_PEER_NONE 0xFFFFu

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

/** Where a frame came from: its interface plus an optional sub-peer. */
typedef struct {
    msg_iface_t iface;        /**< source interface                       */
    uint16_t    peer;         /**< source peer (BLE conn_handle), or
                                   MSG_PEER_NONE for single-port sources   */
} msg_origin_t;

/**
 * Sink callback: deliver one frame to a destination interface.
 * @p origin identifies the producer; the sink uses it to avoid echoing the
 * frame back to its originator (see the file header).
 */
typedef void (*msg_sink_fn_t)(const msg_origin_t *origin,
                              const uint8_t *data, size_t len);

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
 * Push one received frame into the router, tagged with its source interface
 * and no specific peer (MSG_PEER_NONE). Use this for single-port sources
 * (USB CDC, UART Radio, UART Lora).
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM if the router buffer
 *         is full (the frame is dropped with a warning in that case).
 */
esp_err_t msg_submit(msg_iface_t src, const uint8_t *data, size_t len);

/**
 * Push one received frame into the router with a full origin (interface +
 * peer). BLE uses this, passing the conn_handle as @p origin->peer so the BLE
 * sink can exclude the sender via nordic_uart_send_except().
 */
esp_err_t msg_submit_from(const msg_origin_t *origin,
                          const uint8_t *data, size_t len);

/**
 * Deliver one frame to a single destination (no-op if that sink is not
 * registered). Intended for use inside the per-source routing switch when you
 * want to send to an explicit set of destinations instead of "all". The sink
 * still receives @p origin so it can honour self-exclusion.
 */
void msg_router_send_to(msg_iface_t dest, const msg_origin_t *origin,
                        const uint8_t *data, size_t len);

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
