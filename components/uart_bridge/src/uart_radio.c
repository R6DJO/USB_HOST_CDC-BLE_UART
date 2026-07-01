/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART Radio implementation: auto-baud detection + transparent bridge.
 */
#include "uart_radio.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#include "msg_router.h"

static const char *TAG = "UART_RADIO";

/* Baud rates probed, in order of attempt. */
static const uint32_t PROBE_BAUDS[] = {9600, 115200};
#define PROBE_BAUD_COUNT   (sizeof(PROBE_BAUDS) / sizeof(PROBE_BAUDS[0]))

/* AT probe tuning. */
#define AT_CMD             "AT\r\n"
#define AT_RESP_TIMEOUT_MS 800   /**< how long to wait for an "OK" reply */
#define PROBE_RETRY_DELAY  2000  /**< delay before re-probing when nothing replied */

/* RX task. */
#define RX_TASK_STACK      4096
#define RX_TASK_PRIO       6
/* Auto-baud task. */
#define AB_TASK_STACK      4096
#define AB_TASK_PRIO       4

static uart_port_t  s_port;
static QueueHandle_t s_evt_queue;
static volatile uint32_t s_baud;  /**< 0 until auto-baud succeeds */

/* Forward decl: the RX task is spawned by the auto-baud task once a baud is
 * confirmed, so the two never touch the UART at the same time. */
static void uart_radio_rx_task(void *arg);

/* --------------------------------------------------------------- sink (TX) */

static void uart_radio_sink(const msg_origin_t *origin, const uint8_t *data, size_t len)
{
    if (origin->iface == MSG_IF_UART_RADIO) {
        return; /* don't echo back to the radio UART */
    }
    if (s_baud == 0) {
        return; /* baud not detected yet: nowhere to send */
    }
    uart_write_bytes(s_port, data, len);
}

/* --------------------------------------------------------------- auto-baud */

static bool resp_contains_ok(const uint8_t *buf, int len)
{
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == 'O' && buf[i + 1] == 'K') {
            return true;
        }
    }
    return false;
}

/** Send "AT" at @p baud and report whether a valid reply was received. */
static bool probe_baud(uint32_t baud, uint8_t *scratch, size_t scratch_len)
{
    if (uart_set_baudrate(s_port, baud) != ESP_OK) {
        return false;
    }
    uart_flush_input(s_port);
    /* let the new line coding settle and drain any stale event */
    vTaskDelay(pdMS_TO_TICKS(50));
    while (xQueueReceive(s_evt_queue, NULL, 0)) { }

    uart_write_bytes(s_port, AT_CMD, sizeof(AT_CMD) - 1);
    uart_wait_tx_done(s_port, pdMS_TO_TICKS(100));

    int n = uart_read_bytes(s_port, scratch, scratch_len - 1,
                            pdMS_TO_TICKS(AT_RESP_TIMEOUT_MS));
    if (n <= 0) {
        return false;
    }
    scratch[n] = '\0';
    return resp_contains_ok(scratch, n);
}

/**
 * Tries each candidate baud in turn. On success flushes the link, stores the
 * working baud, spawns the RX task and self-deletes. Otherwise waits and
 * retries (handles a device connected after boot).
 */
static void uart_radio_autobaud_task(void *arg)
{
    (void)arg;
    size_t scratch_len = CONFIG_UART_RADIO_BUF_SIZE;
    uint8_t *scratch = malloc(scratch_len);
    if (scratch == NULL) {
        ESP_LOGE(TAG, "autobaud malloc fail");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        for (size_t i = 0; i < PROBE_BAUD_COUNT; i++) {
            if (probe_baud(PROBE_BAUDS[i], scratch, scratch_len)) {
                s_baud = PROBE_BAUDS[i];
                uart_flush_input(s_port);
                while (xQueueReceive(s_evt_queue, NULL, 0)) { }
                ESP_LOGI(TAG, "Baud detected: %lu bps", (unsigned long)s_baud);

                /* RX task starts only now, after probing is done and the link
                 * is flushed, so it never races with the probe reads above. */
                xTaskCreate(uart_radio_rx_task, "uart_radio_rx",
                            RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL);
                free(scratch);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGD(TAG, "No AT reply at %lu bps", (unsigned long)PROBE_BAUDS[i]);
        }
        ESP_LOGI(TAG, "No AT device detected, retrying in %d ms", PROBE_RETRY_DELAY);
        vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_DELAY));
    }
}

/* --------------------------------------------------------------- RX task */

static void uart_radio_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started (%lu bps)", (unsigned long)s_baud);

    size_t buf_len = CONFIG_UART_RADIO_BUF_SIZE;
    uint8_t *buf = malloc(buf_len);
    if (buf == NULL) {
        ESP_LOGE(TAG, "rx malloc fail");
        vTaskDelete(NULL);
        return;
    }

    uart_event_t event;
    for (;;) {
        if (!xQueueReceive(s_evt_queue, &event, portMAX_DELAY)) {
            continue;
        }
        if (event.type != UART_DATA || event.size == 0) {
            continue;
        }
        size_t want = event.size;
        if (want > buf_len) {
            want = buf_len;
        }
        int n = uart_read_bytes(s_port, buf, want, portMAX_DELAY);
        if (n > 0) {
            msg_submit(MSG_IF_UART_RADIO, buf, (size_t)n);
        }
    }
}

/* --------------------------------------------------------------- init */

esp_err_t uart_radio_init(void)
{
    s_port = (uart_port_t)CONFIG_UART_RADIO_NUM;
    s_baud = 0;

    const uart_config_t cfg = {
        .baud_rate  = PROBE_BAUDS[0], /* changed later by auto-baud */
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(s_port,
                                       CONFIG_UART_RADIO_BUF_SIZE * 2,
                                       CONFIG_UART_RADIO_BUF_SIZE * 2,
                                       20, &s_evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(s_port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_port,
                                 CONFIG_UART_RADIO_TX_GPIO,
                                 CONFIG_UART_RADIO_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    msg_router_register_sink(MSG_IF_UART_RADIO, uart_radio_sink);

    xTaskCreate(uart_radio_autobaud_task, "uart_radio_ab", AB_TASK_STACK,
                NULL, AB_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "Driver installed (UART%d TX=GPIO%d RX=GPIO%d), probing baud...",
             s_port, CONFIG_UART_RADIO_TX_GPIO, CONFIG_UART_RADIO_RX_GPIO);
    return ESP_OK;
}

uint32_t uart_radio_baud(void)
{
    return s_baud;
}
