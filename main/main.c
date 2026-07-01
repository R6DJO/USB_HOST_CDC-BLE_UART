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
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

#include "nordic_uart_multi.h"
#include "msg_router.h"
#include "uart_radio.h"
#include "uart_lora.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#define USB_HOST_PRIORITY      (20)
#define MD9600_USB_DEVICE_VID  (0x1FC9)
#define MD9600_USB_DEVICE_PID  (0x0094) /* 0x1FC9:0x0094 (MD9600 CDC device) */
#define USB_TX_TIMEOUT_MS      (1000)

static const char *TAG = "DMR-RADIO";
static SemaphoreHandle_t device_disconnected_sem;
cdc_acm_dev_hdl_t cdc_dev = NULL;

/* ----------------------------------------------------------- router sinks */

/** Deliver a frame to the USB CDC device (the radio). Drops if not open. */
static void usb_cdc_sink(const uint8_t *data, size_t len)
{
    if (cdc_dev == NULL) {
        return;
    }
    esp_err_t err = cdc_acm_host_data_tx_blocking(cdc_dev, data, len,
                                                  USB_TX_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW("ROUTER->USB", "USB CDC tx failed: %s", esp_err_to_name(err));
    }
}

/** Deliver a frame to all subscribed BLE peers. */
static void ble_sink(const uint8_t *data, size_t len)
{
    esp_err_t err = nordic_uart_send(data, len);
    if (err != ESP_OK) {
        ESP_LOGW("ROUTER->BLE", "BLE send failed: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Data received from the USB CDC device (radio -> router).
 */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    msg_submit(MSG_IF_USB_CDC, data, data_len);
    ESP_LOGI("USB->ROUTER", "radio -> %u byte(s)", data_len);
    return true;
}

/**
 * @brief Device event callback
 */
static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error has occurred, err_no = %i", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device suddenly disconnected");
        ESP_ERROR_CHECK(cdc_acm_host_close(event->data.cdc_hdl));
        xSemaphoreGive(device_disconnected_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        ESP_LOGW(TAG, "Unsupported CDC event: %i", event->type);
        break;
    }
}

/**
 * @brief USB Host library handling task
 */
static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
        }
    }
}

/**
 * @brief BLE -> router task.
 *
 * Drains the Nordic UART RX ring buffer (data written by any BLE peer) and
 * submits each frame to the message router tagged with MSG_IF_BLE.
 */
void ble_to_router_task(void *parameter)
{
    (void)parameter;
    for (;;) {
        if (nordic_uart_rx_buf_handle == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t item_size = 0;
        const nordic_uart_rx_item_t *item =
            (const nordic_uart_rx_item_t *)xRingbufferReceive(
                nordic_uart_rx_buf_handle, &item_size, portMAX_DELAY);

        if (item == NULL) {
            continue;
        }

        ESP_LOGI("BLE->ROUTER", "peer=%u -> %u byte(s)",
                 item->conn_handle, item->len);
        msg_submit(MSG_IF_BLE, item->data, item->len);

        vRingbufferReturnItem(nordic_uart_rx_buf_handle, (void *)item);
    }

    vTaskDelete(NULL);
}

/**
 * @brief Main application
 */
void app_main(void)
{
    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    /* --- Message router: create queue, register sinks, start task -------- */
    ESP_ERROR_CHECK(msg_router_init());
    msg_router_register_sink(MSG_IF_USB_CDC, usb_cdc_sink);
    msg_router_register_sink(MSG_IF_BLE,     ble_sink);

    /* --- UART interfaces (self-register their sinks) --------------------- */
    ESP_ERROR_CHECK(uart_radio_init());
    ESP_ERROR_CHECK(uart_lora_init());

    ESP_ERROR_CHECK(msg_router_start());

    /* --- BLE Nordic UART peripheral -------------------------------------- */
    nordic_uart_start("DMR-RADIO");
    BaseType_t task_created = xTaskCreate(ble_to_router_task, "ble2rt", 5000,
                                          NULL, 1, NULL);
    assert(task_created == pdTRUE);

    /* --- USB Host + CDC-ACM ---------------------------------------------- */
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096,
                                xTaskGetCurrentTaskHandle(),
                                USB_HOST_PRIORITY, NULL);
    assert(task_created == pdTRUE);

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx,
    };

    while (true) {
        ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X...",
                 MD9600_USB_DEVICE_VID, MD9600_USB_DEVICE_PID);
        esp_err_t err = cdc_acm_host_open(MD9600_USB_DEVICE_VID,
                                          MD9600_USB_DEVICE_PID, 0,
                                          &dev_config, &cdc_dev);
        if (ESP_OK != err) {
            ESP_LOGI(TAG, "Failed to open device");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Configure line coding: 115200 8N1 */
        cdc_acm_line_coding_t line_coding;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));

        line_coding.dwDTERate = 115200;
        line_coding.bDataBits = 8;
        line_coding.bParityType = 0;
        line_coding.bCharFormat = 1;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(cdc_dev, &line_coding));

        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
        ESP_LOGI(TAG, "Line Get: Rate: %" PRIu32 ", Stop bits: %" PRIu8
                 ", Parity: %" PRIu8 ", Databits: %" PRIu8,
                 line_coding.dwDTERate, line_coding.bCharFormat,
                 line_coding.bParityType, line_coding.bDataBits);

        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(cdc_dev, true, false));

        ESP_LOGI(TAG, "Connected CDC ACM device 0x%04X:0x%04X (%u BLE peer(s))...",
                 MD9600_USB_DEVICE_VID, MD9600_USB_DEVICE_PID,
                 nordic_uart_client_count());

        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
    }
}
