/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Central message router implementation.
 */
#include "msg_router.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "MSG_ROUTER";

/* FreeRTOS task params. */
#define MSG_ROUTER_TASK_STACK  4096
#define MSG_ROUTER_TASK_PRIO   8

/* How long msg_submit() waits for room in the ring buffer before dropping. */
#define MSG_SUBMIT_TIMEOUT_MS  20

/** One routed frame. Payload bytes follow the header (flexible array member). */
typedef struct {
    msg_origin_t origin;/**< source interface + peer          */
    uint16_t     len;   /**< bytes in data[]                   */
    uint8_t      data[];/**< raw payload, binary-safe          */
} msg_item_t;

static RingbufHandle_t s_buf;
static msg_sink_fn_t   s_sinks[MSG_IF_COUNT];

esp_err_t msg_router_init(void)
{
    if (s_buf != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_buf = xRingbufferCreate(CONFIG_MSG_ROUTER_BUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer (%d bytes)",
                 CONFIG_MSG_ROUTER_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    memset(s_sinks, 0, sizeof s_sinks);
    ESP_LOGI(TAG, "Initialized (buffer %d bytes)", CONFIG_MSG_ROUTER_BUF_SIZE);
    return ESP_OK;
}

esp_err_t msg_router_register_sink(msg_iface_t dest, msg_sink_fn_t fn)
{
    if (dest >= MSG_IF_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_sinks[dest] = fn;
    return ESP_OK;
}

esp_err_t msg_submit_from(const msg_origin_t *origin,
                          const uint8_t *data, size_t len)
{
    if (s_buf == NULL || origin == NULL || origin->iface >= MSG_IF_COUNT ||
        data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0xFFFF) {
        len = 0xFFFF; /* header field is uint16_t */
    }

    size_t item_size = sizeof(msg_item_t) + len;
    msg_item_t *item = malloc(item_size);
    if (item == NULL) {
        ESP_LOGE(TAG, "submit malloc fail (%u bytes from if=%d)",
                 (unsigned)item_size, origin->iface);
        return ESP_ERR_NO_MEM;
    }
    item->origin = *origin;
    item->len = (uint16_t)len;
    memcpy(item->data, data, len);

    if (xRingbufferSend(s_buf, item, item_size,
                        pdMS_TO_TICKS(MSG_SUBMIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Router buffer full, dropping %u byte(s) from if=%d",
                 (unsigned)len, origin->iface);
        free(item);
        return ESP_ERR_NO_MEM;
    }
    free(item); /* xRingbufferSend copied the contents */
    return ESP_OK;
}

esp_err_t msg_submit(msg_iface_t src, const uint8_t *data, size_t len)
{
    msg_origin_t origin = { .iface = src, .peer = MSG_PEER_NONE };
    return msg_submit_from(&origin, data, len);
}

/* --------------------------------------------------------------- routing */

/**
 * Default delivery: hand the frame to EVERY registered sink. Each sink is
 * responsible for not echoing it back to its own originator (single-port
 * interfaces skip their own interface; BLE skips only the sender peer). This
 * keeps the router generic while still enforcing "no loop-back".
 */
static void deliver_to_all(const msg_origin_t *origin,
                           const uint8_t *data, size_t len)
{
    for (int d = 0; d < MSG_IF_COUNT; d++) {
        if (s_sinks[d]) {
            s_sinks[d](origin, data, len);
        }
    }
}

void msg_router_send_to(msg_iface_t dest, const msg_origin_t *origin,
                        const uint8_t *data, size_t len)
{
    if (dest < MSG_IF_COUNT && s_sinks[dest]) {
        s_sinks[dest](origin, data, len);
    }
}

/**
 * Per-source routing policy.
 *
 * Default behaviour: deliver to all interfaces (sinks self-exclude the
 * originator). To restrict a source to a specific set of destinations, edit
 * the matching case and forward explicitly with msg_router_send_to(), e.g.:
 *
 *     case MSG_IF_UART_LORA:
 *         // Lora -> BLE only
 *         msg_router_send_to(MSG_IF_BLE, origin, data, len);
 *         break;
 *
 * To completely suppress BLE peer fan-out (so BLE clients are isolated from
 * each other), simply don't call the BLE sink from the MSG_IF_BLE case, e.g.:
 *
 *     case MSG_IF_BLE:
 *         msg_router_send_to(MSG_IF_USB_CDC,    origin, data, len);
 *         msg_router_send_to(MSG_IF_UART_RADIO, origin, data, len);
 *         msg_router_send_to(MSG_IF_UART_LORA,  origin, data, len);
 *         break;
 */
static void route_message(const msg_origin_t *origin,
                          const uint8_t *data, size_t len)
{
    switch (origin->iface) {
    case MSG_IF_USB_CDC:
        deliver_to_all(origin, data, len);
        break;

    case MSG_IF_BLE:
        deliver_to_all(origin, data, len);
        break;

    case MSG_IF_UART_RADIO:
        deliver_to_all(origin, data, len);
        break;

    case MSG_IF_UART_LORA:
        deliver_to_all(origin, data, len);
        break;

    default:
        ESP_LOGW(TAG, "Unknown source if=%d, dropping %u byte(s)",
                 origin->iface, (unsigned)len);
        break;
    }
}

void msg_routing_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Routing task started");
    for (;;) {
        size_t item_size = 0;
        const msg_item_t *item = (const msg_item_t *)xRingbufferReceive(
            s_buf, &item_size, portMAX_DELAY);
        if (item == NULL) {
            continue;
        }

        ESP_LOGD(TAG, "route if=%d peer=%u -> %u byte(s)",
                 item->origin.iface, item->origin.peer, item->len);
        route_message(&item->origin, item->data, item->len);

        vRingbufferReturnItem(s_buf, (void *)item);
    }
}

esp_err_t msg_router_start(void)
{
    if (s_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreate(msg_routing_task, "msg_routing",
                                MSG_ROUTER_TASK_STACK, NULL,
                                MSG_ROUTER_TASK_PRIO, NULL);
    return (ok == pdTRUE) ? ESP_OK : ESP_FAIL;
}
