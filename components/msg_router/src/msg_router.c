/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Central message router implementation.
 *
 * Delivery model (variant C -- asynchronous per-sink queues):
 *   - Producers push frames into one central ring buffer (msg_submit*).
 *   - The msg_routing task drains it and pushes each frame, NON-blocking and
 *     drop-on-full, into the PRIVATE queue of every target sink.
 *   - Each registered sink owns a dedicated delivery task that drains its own
 *     queue and calls the real delivery function. That function may block
 *     freely (USB tx, BLE notify, UART write) -- only that one sink is
 *     affected; the router and every other sink keep running.
 *
 * Sink authors see no change: msg_sink_fn_t is still a plain function. The
 * async isolation is provided transparently by msg_router_register_sink().
 */
#include "msg_router.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MSG_ROUTER";

/* Central routing (dispatch) task. */
#define MSG_ROUTER_TASK_STACK  4096
#define MSG_ROUTER_TASK_PRIO   8

/* Per-sink delivery task. Lower than the router so dispatch never starves. */
#define SINK_TASK_PRIO         5

/* How long msg_submit() waits for room in the central buffer before dropping. */
#define MSG_SUBMIT_TIMEOUT_MS  20

/** One routed frame. Payload bytes follow the header (flexible array member).
 *  Used both by the central buffer and by each per-sink queue. */
typedef struct {
    msg_origin_t origin;/**< source interface + peer          */
    uint16_t     len;   /**< bytes in data[]                   */
    uint8_t      data[];/**< raw payload, binary-safe          */
} msg_item_t;

typedef struct {
    msg_sink_fn_t   deliver;  /**< real delivery fn (may block)   */
    RingbufHandle_t q;        /**< private per-sink queue         */
} sink_slot_t;

static RingbufHandle_t s_buf;
static sink_slot_t     s_sinks[MSG_IF_COUNT];

/* Per-sink task names (FreeRTOS caps these at configMAX_TASK_NAME_LEN). */
static const char *const SINK_TASK_NAMES[MSG_IF_COUNT] = {
    [MSG_IF_USB_CDC]    = "sink_usb",
    [MSG_IF_BLE]        = "sink_ble",
    [MSG_IF_UART_RADIO] = "sink_urad",
    [MSG_IF_UART_LORA]  = "sink_ulora",
};

/* ----------------------------------------------------------- drop statistics */
/* Cumulative per-interface drop counters, touched from many tasks (producers
 * in msg_submit, the routing task in sink_enqueue, the esp_timer task in the
 * logger). Guarded by a critical section; each critical section is tiny (one
 * increment / one memcpy). */
static const char *const IFACE_NAMES[MSG_IF_COUNT] = {
    [MSG_IF_USB_CDC]    = "usb",
    [MSG_IF_BLE]        = "ble",
    [MSG_IF_UART_RADIO] = "urad",
    [MSG_IF_UART_LORA]  = "ulora",
};

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_submit_drops[MSG_IF_COUNT]; /* central-buffer drops, per source */
static uint32_t s_sink_drops[MSG_IF_COUNT];   /* per-sink queue drops, per dest   */
static uint32_t s_last_sub[MSG_IF_COUNT];     /* last logged (change detect)      */
static uint32_t s_last_snk[MSG_IF_COUNT];
static esp_timer_handle_t s_stats_timer;

static void stats_inc_submit(msg_iface_t src)
{
    if (src >= MSG_IF_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    s_submit_drops[src]++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void stats_inc_sink(msg_iface_t dest)
{
    if (dest >= MSG_IF_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_stats_lock);
    s_sink_drops[dest]++;
    portEXIT_CRITICAL(&s_stats_lock);
}

/** Log cumulative drop totals, but only if any counter changed since the last
 *  call (so a quiet system produces no output). Public via the header. */
void msg_router_log_stats(void)
{
    uint32_t sub[MSG_IF_COUNT], snk[MSG_IF_COUNT];
    portENTER_CRITICAL(&s_stats_lock);
    memcpy(sub, s_submit_drops, sizeof sub);
    memcpy(snk, s_sink_drops, sizeof snk);
    portEXIT_CRITICAL(&s_stats_lock);

    bool changed = false;
    for (int i = 0; i < MSG_IF_COUNT; i++) {
        if (sub[i] != s_last_sub[i] || snk[i] != s_last_snk[i]) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    /* Compact one-liner: drop totals: submit{usb=N ble=N ...} sink{...} */
    char buf[160];
    int off = snprintf(buf, sizeof buf, "drop totals: submit{");
    for (int i = 0; i < MSG_IF_COUNT; i++) {
        if (off >= (int)sizeof buf) break;
        off += snprintf(buf + off, sizeof buf - off, "%s%s=%lu",
                        i ? " " : "", IFACE_NAMES[i], (unsigned long)sub[i]);
    }
    if (off < (int)sizeof buf) {
        off += snprintf(buf + off, sizeof buf - off, "} sink{");
    }
    for (int i = 0; i < MSG_IF_COUNT; i++) {
        if (off >= (int)sizeof buf) break;
        off += snprintf(buf + off, sizeof buf - off, "%s%s=%lu",
                        i ? " " : "", IFACE_NAMES[i], (unsigned long)snk[i]);
    }
    if (off < (int)sizeof buf) {
        snprintf(buf + off, sizeof buf - off, "}");
    }
    ESP_LOGI(TAG, "%s", buf);

    for (int i = 0; i < MSG_IF_COUNT; i++) {
        s_last_sub[i] = sub[i];
        s_last_snk[i] = snk[i];
    }
}

static void stats_timer_cb(void *arg)
{
    (void)arg;
    msg_router_log_stats();
}

/* ----------------------------------------------------------- sink delivery */

/** Drain one sink's queue and call its delivery function. Runs in its own
 *  task so the (possibly blocking) delivery cannot stall the router. */
static void sink_task(void *arg)
{
    msg_iface_t   iface    = (msg_iface_t)(intptr_t)arg;
    msg_sink_fn_t deliver  = s_sinks[iface].deliver;
    RingbufHandle_t q      = s_sinks[iface].q;

    for (;;) {
        size_t item_size = 0;
        const msg_item_t *item = (const msg_item_t *)xRingbufferReceive(
            q, &item_size, portMAX_DELAY);
        if (item == NULL) {
            continue;
        }
        deliver(&item->origin, item->data, item->len);
        vRingbufferReturnItem(q, (void *)item);
    }
}

/* --------------------------------------------------------------- lifecycle */

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
    if (dest >= MSG_IF_COUNT || fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sinks[dest].deliver != NULL) {
        return ESP_ERR_INVALID_STATE; /* already registered */
    }

    s_sinks[dest].q = xRingbufferCreate(CONFIG_MSG_ROUTER_SINK_QUEUE_SIZE,
                                        RINGBUF_TYPE_NOSPLIT);
    if (s_sinks[dest].q == NULL) {
        ESP_LOGE(TAG, "Failed to create sink queue if=%d (%d bytes)",
                 dest, CONFIG_MSG_ROUTER_SINK_QUEUE_SIZE);
        return ESP_ERR_NO_MEM;
    }
    s_sinks[dest].deliver = fn;

    BaseType_t ok = xTaskCreate(sink_task, SINK_TASK_NAMES[dest],
                                CONFIG_MSG_ROUTER_SINK_TASK_STACK,
                                (void *)(intptr_t)dest, SINK_TASK_PRIO, NULL);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create sink task if=%d", dest);
        vRingbufferDelete(s_sinks[dest].q);
        s_sinks[dest].q = NULL;
        s_sinks[dest].deliver = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------- submit */

esp_err_t msg_submit_from(const msg_origin_t *origin,
                          const uint8_t *data, size_t len)
{
    if (s_buf == NULL || origin == NULL || origin->iface >= MSG_IF_COUNT ||
        data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0xFFFF) {
        return ESP_ERR_INVALID_SIZE; /* header field is uint16_t */
    }

    /* Zero-copy submit into the central buffer. */
    size_t item_size = sizeof(msg_item_t) + len;
    msg_item_t *item;
    if (xRingbufferSendAcquire(s_buf, (void **)&item, item_size,
                               pdMS_TO_TICKS(MSG_SUBMIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Router buffer full, dropping %u byte(s) from if=%d",
                 (unsigned)len, origin->iface);
        stats_inc_submit(origin->iface);
        return ESP_ERR_NO_MEM;
    }
    item->origin = *origin;
    item->len = (uint16_t)len;
    memcpy(item->data, data, len);
    xRingbufferSendComplete(s_buf, item);
    return ESP_OK;
}

esp_err_t msg_submit(msg_iface_t src, const uint8_t *data, size_t len)
{
    msg_origin_t origin = { .iface = src, .peer = MSG_PEER_NONE };
    return msg_submit_from(&origin, data, len);
}

/* --------------------------------------------------------------- dispatch */

/** Push one frame into a sink's private queue (non-blocking, drop-on-full).
 *  Keeping this non-blocking is what guarantees a slow sink cannot stall the
 *  routing task or any other sink. */
static void sink_enqueue(msg_iface_t dest, const msg_origin_t *origin,
                         const uint8_t *data, size_t len)
{
    RingbufHandle_t q = s_sinks[dest].q;
    if (q == NULL) {
        return; /* no sink registered for this destination */
    }

    size_t item_size = sizeof(msg_item_t) + len;
    msg_item_t *item;
    if (xRingbufferSendAcquire(q, (void **)&item, item_size, 0) != pdTRUE) {
        /* Queue full: drop the newest frame for THIS sink only. */
        ESP_LOGW(TAG, "sink if=%d queue full, dropping %u byte(s)",
                 dest, (unsigned)len);
        stats_inc_sink(dest);
        return;
    }
    item->origin = *origin;
    item->len = (uint16_t)len;
    memcpy(item->data, data, len);
    xRingbufferSendComplete(q, item);
}

/**
 * Default delivery: enqueue the frame into EVERY registered sink's queue.
 * Each sink still self-excludes its own originator inside its delivery fn
 * (single-port interfaces skip their own iface; BLE skips only the sender).
 */
static void deliver_to_all(const msg_origin_t *origin,
                           const uint8_t *data, size_t len)
{
    for (int d = 0; d < MSG_IF_COUNT; d++) {
        if (s_sinks[d].deliver) {
            sink_enqueue((msg_iface_t)d, origin, data, len);
        }
    }
}

void msg_router_send_to(msg_iface_t dest, const msg_origin_t *origin,
                        const uint8_t *data, size_t len)
{
    if (dest < MSG_IF_COUNT && s_sinks[dest].deliver) {
        sink_enqueue(dest, origin, data, len);
    }
}

/* --------------------------------------------------------------- routing */

/**
 * Per-source routing policy.
 *
 * Default: enqueue into every sink. To restrict a source to specific
 * destinations, switch over origin->iface and forward explicitly with
 * msg_router_send_to(), e.g.:
 *
 *     switch (origin->iface) {
 *     case MSG_IF_UART_LORA:            // Lora -> BLE only
 *         msg_router_send_to(MSG_IF_BLE, origin, data, len);
 *         break;
 *     default:
 *         deliver_to_all(origin, data, len);
 *         break;
 *     }
 *
 * Both paths go through the per-sink queues, so they never block.
 *
 * Frames with an out-of-range iface never reach here: msg_submit_from()
 * rejects them before they enter the central buffer.
 */
static void route_message(const msg_origin_t *origin,
                          const uint8_t *data, size_t len)
{
    deliver_to_all(origin, data, len);
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
        /* Dispatch is non-blocking: each sink gets its own copy in its queue,
         * so we can return the central-buffer item immediately. */
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
    if (ok != pdTRUE) {
        return ESP_FAIL;
    }

    /* Periodic drop-stats logger. 0 disables the timer; counters are still
     * kept and readable on demand via msg_router_log_stats(). */
    if (CONFIG_MSG_ROUTER_STATS_PERIOD_MS > 0) {
        const esp_timer_create_args_t args = {
            .callback = stats_timer_cb,
            .name = "rt_stats",
        };
        if (esp_timer_create(&args, &s_stats_timer) == ESP_OK) {
            esp_timer_start_periodic(s_stats_timer,
                                     CONFIG_MSG_ROUTER_STATS_PERIOD_MS * 1000ULL);
        }
    }
    return ESP_OK;
}
