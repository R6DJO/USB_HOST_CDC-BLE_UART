/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * UART Lora implementation: fixed-baud transparent bridge.
 */
#include "uart_lora.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#include "msg_router.h"

static const char *TAG = "UART_LORA";

#define RX_TASK_STACK 4096
#define RX_TASK_PRIO  6

static uart_port_t   s_port;
static QueueHandle_t s_evt_queue;

/* --------------------------------------------------------------- sink (TX) */

static void uart_lora_sink(const uint8_t *data, size_t len)
{
    uart_write_bytes(s_port, data, len);
}

/* --------------------------------------------------------------- RX task */

static void uart_lora_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started (%d bps)", CONFIG_UART_LORA_BAUDRATE);

    size_t buf_len = CONFIG_UART_LORA_BUF_SIZE;
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
            msg_submit(MSG_IF_UART_LORA, buf, (size_t)n);
        }
    }
}

/* --------------------------------------------------------------- init */

esp_err_t uart_lora_init(void)
{
    s_port = (uart_port_t)CONFIG_UART_LORA_NUM;

    const uart_config_t cfg = {
        .baud_rate  = CONFIG_UART_LORA_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(s_port,
                                       CONFIG_UART_LORA_BUF_SIZE * 2,
                                       CONFIG_UART_LORA_BUF_SIZE * 2,
                                       20, &s_evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(s_port, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(s_port,
                                 CONFIG_UART_LORA_TX_GPIO,
                                 CONFIG_UART_LORA_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    msg_router_register_sink(MSG_IF_UART_LORA, uart_lora_sink);

    xTaskCreate(uart_lora_rx_task, "uart_lora_rx", RX_TASK_STACK,
                NULL, RX_TASK_PRIO, NULL);
    ESP_LOGI(TAG, "Driver installed (UART%d TX=GPIO%d RX=GPIO%d @%d bps)",
             s_port, CONFIG_UART_LORA_TX_GPIO, CONFIG_UART_LORA_RX_GPIO,
             CONFIG_UART_LORA_BAUDRATE);
    return ESP_OK;
}
