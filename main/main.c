/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
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
#include "nimble-nordic-uart.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#define USB_HOST_PRIORITY (20)
#define MD9600_USB_DEVICE_VID (0x1FC9)
#define MD9600_USB_DEVICE_PID (0x0094) // 0x1FC9:0x0094 (MD9600 CDC device)
#define USB_TX_TIMEOUT_MS (1000)

static const char *TAG = "DMR-RADIO";
static SemaphoreHandle_t device_disconnected_sem;
cdc_acm_dev_hdl_t cdc_dev = NULL;

/**
 * @brief Data received callback
 *
 * @param[in] data     Pointer to received data
 * @param[in] data_len Length of received data in bytes
 * @param[in] arg      Argument we passed to the device open function
 * @return
 *   true:  We have processed the received data
 *   false: We expect more data
 */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    static char mbuf[CONFIG_NORDIC_UART_MAX_LINE_LENGTH + 1];
    strncpy(mbuf, (char *)data, data_len);
    mbuf[data_len] = '\0';

    // ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    esp_err_t err = nordic_uart_send(mbuf);
    if (ESP_OK != err)
    {
        ESP_LOGW("UART->BLE", "Failed to sent to BLE UART");
    }
    if (mbuf[data_len - 2] == '\r')
    {
        mbuf[data_len - 2] = '\0';
    }
    ESP_LOGI("UART->BLE", "%s", mbuf);
    return true;
}

/**
 * @brief Device event callback
 *
 * Apart from handling device disconnection it doesn't do anything useful
 *
 * @param[in] event    Device event type and data
 * @param[in] user_ctx Argument we passed to the device open function
 */
static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type)
    {
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
 *
 * @param arg Unused
 */
static void usb_lib_task(void *arg)
{
    while (1)
    {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG, "USB: All devices freed");
            // Continue handling USB events to allow device reconnection
        }
    }
}

void echoTask(void *parameter)
{
    static char mbuf[CONFIG_NORDIC_UART_MAX_LINE_LENGTH + 1];

    for (;;)
    {
        size_t item_size;
        if (nordic_uart_rx_buf_handle)
        {
            const char *item = (char *)xRingbufferReceive(nordic_uart_rx_buf_handle, &item_size, portMAX_DELAY);

            if (item)
            {
                strncpy(mbuf, (char *)item, item_size);
                mbuf[item_size] = '\0';
                ESP_LOGI("BLE->UART", "%s", mbuf);
                esp_err_t err = cdc_acm_host_data_tx_blocking(cdc_dev, (const uint8_t *)mbuf, strlen(mbuf), USB_TX_TIMEOUT_MS);
                if (ESP_OK != err)
                {
                    ESP_LOGW("BLE->UART", "Failed send to USB UART");
                }
                vRingbufferReturnItem(nordic_uart_rx_buf_handle, (void *)item);
            }
        }
        else
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    vTaskDelete(NULL);
}

/**
 * @brief Main application
 *
 * Here we open a USB CDC device and send some data to it
 */
void app_main(void)
{
    device_disconnected_sem = xSemaphoreCreateBinary();
    assert(device_disconnected_sem);

    // Install USB Host driver. Should only be called once in entire application
    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // Create a task that will handle USB library events
    BaseType_t task_created = xTaskCreate(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), USB_HOST_PRIORITY, NULL);
    assert(task_created == pdTRUE);

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 1000,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx};

    nordic_uart_start("DMR-RADIO", NULL);
    task_created = xTaskCreate(echoTask, "echoTask", 5000, NULL, 1, NULL);
    assert(task_created == pdTRUE);

    while (true)
    {
        // Open USB device from tusb_serial_device example example. Either single or dual port configuration.
        ESP_LOGI(TAG, "Opening CDC ACM device 0x%04X:0x%04X...", MD9600_USB_DEVICE_VID, MD9600_USB_DEVICE_PID);
        esp_err_t err = cdc_acm_host_open(MD9600_USB_DEVICE_VID, MD9600_USB_DEVICE_PID, 0, &dev_config, &cdc_dev);
        if (ESP_OK != err)
        {
            ESP_LOGI(TAG, "Failed to open device");
            continue;
        }
        // cdc_acm_host_desc_print(cdc_dev);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Test sending and receiving: responses are handled in handle_rx callback
        // ESP_ERROR_CHECK(cdc_acm_host_data_tx_blocking(cdc_dev, (const uint8_t *)EXAMPLE_TX_STRING, strlen(EXAMPLE_TX_STRING), EXAMPLE_TX_TIMEOUT_MS));
        // vTaskDelay(pdMS_TO_TICKS(100));

        // Test Line Coding commands: Get current line coding, change it 9600 7N1 and read again
        ESP_LOGI(TAG, "Setting up line coding");

        cdc_acm_line_coding_t line_coding;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
        // ESP_LOGI(TAG, "Line Get: Rate: %" PRIu32 ", Stop bits: %" PRIu8 ", Parity: %" PRIu8 ", Databits: %" PRIu8 "",
        //          line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        line_coding.dwDTERate = 115200;
        line_coding.bDataBits = 8;
        line_coding.bParityType = 0;
        line_coding.bCharFormat = 1;
        ESP_ERROR_CHECK(cdc_acm_host_line_coding_set(cdc_dev, &line_coding));
        // ESP_LOGI(TAG, "Line Set: Rate: %" PRIu32 ", Stop bits: %" PRIu8 ", Parity: %" PRIu8 ", Databits: %" PRIu8 "",
        //          line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        ESP_ERROR_CHECK(cdc_acm_host_line_coding_get(cdc_dev, &line_coding));
        ESP_LOGI(TAG, "Line Get: Rate: %" PRIu32 ", Stop bits: %" PRIu8 ", Parity: %" PRIu8 ", Databits: %" PRIu8 "",
                 line_coding.dwDTERate, line_coding.bCharFormat, line_coding.bParityType, line_coding.bDataBits);

        ESP_ERROR_CHECK(cdc_acm_host_set_control_line_state(cdc_dev, true, false));

        // We are done. Wait for device disconnection and start over
        ESP_LOGI(TAG, "Connected CDC ACM device 0x%04X:0x%04X...", MD9600_USB_DEVICE_VID, MD9600_USB_DEVICE_PID);
        xSemaphoreTake(device_disconnected_sem, portMAX_DELAY);
    }
}
