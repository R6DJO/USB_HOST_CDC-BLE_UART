/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * USB CDC-ACM bridge to the message router.
 *
 * Spawns two background tasks:
 *   - usb_lib_task : drains the USB Host library event loop (must run for any
 *                    USB client on this chip).
 *   - usb_cdc_task : opens the configured CDC-ACM device, applies line coding,
 *                    and on disconnect closes + re-opens automatically.
 *
 * Concurrency: cdc_dev is touched from the sink_usb delivery task (TX), the
 * CDC event callback (disconnect) and usb_cdc_task (open/configure). cdc_mutex
 * serializes TX vs. close so the sink never transmits on a handle that is
 * being/has been closed (see comments at the call sites).
 */
#include "usb_cdc.h"

#include <inttypes.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "msg_router.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

static const char *TAG = "USB_CDC";

/* ---- tasks ---- */
#define USB_HOST_PRIORITY      (20)
#define USB_LIB_TASK_STACK     4096
#define USB_CDC_TASK_STACK     4096
#define USB_CDC_TASK_PRIO      5

/* ---- device open / line coding ---- */
#define CDC_OPEN_RETRY_MS      1000   /* delay before re-trying a failed open */
#define CDC_OPEN_TIMEOUT_MS    1000   /* cdc_acm_host_open connection timeout */
#define CDC_OPEN_SETTLE_MS     100    /* let the device settle after open    */
#define CDC_BUF_SIZE           512    /* CDC-ACM in/out buffer size           */

static SemaphoreHandle_t s_disconnected_sem;  /* signaled by the disconnect cb */
static SemaphoreHandle_t s_cdc_mutex;         /* guards cdc_dev (tx vs close)  */
static cdc_acm_dev_hdl_t  s_cdc_dev = NULL;

/* ----------------------------------------------------------- router sink (TX) */

/** Deliver a frame to the USB CDC device (the radio). Drops if not open, and
 *  never echoes a frame back to the radio that came from the radio. */
static void usb_cdc_sink(const msg_origin_t *origin, const uint8_t *data, size_t len)
{
    if (origin->iface == MSG_IF_USB_CDC) {
        return; /* don't loop back to the radio */
    }
    /* Hold the mutex across the (possibly blocking) tx so it cannot race with
     * a concurrent disconnect/close. Worst case a disconnect is delayed by
     * CONFIG_USB_CDC_TX_TIMEOUT_MS until this tx finishes -- bounded, no
     * deadlock (the tx itself has a timeout). */
    xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
    cdc_acm_dev_hdl_t dev = s_cdc_dev;   /* snapshot under lock */
    if (dev != NULL) {
        esp_err_t err = cdc_acm_host_data_tx_blocking(dev, data, len,
                                                      CONFIG_USB_CDC_TX_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CDC tx failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_cdc_mutex);
}

/* ----------------------------------------------------------- CDC callbacks */

/** Data received from the CDC device (radio -> router). */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    (void)arg;
    msg_submit(MSG_IF_USB_CDC, data, data_len);
    ESP_LOGD(TAG, "radio -> %u byte(s)", data_len);
    return true;
}

/** CDC-ACM device event callback (runs in the CDC driver's background task). */
static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error, err_no = %i", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        ESP_LOGI(TAG, "Device suddenly disconnected");
        /* Null cdc_dev under the lock first: this waits for any in-flight sink
         * tx to finish, so by the time close() runs the sink has released the
         * handle -> no use-after-close. close() itself is outside the lock to
         * avoid re-entering the CDC driver while it may need its own task. */
        xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
        s_cdc_dev = NULL;
        xSemaphoreGive(s_cdc_mutex);
        /* Best-effort close: the device is already gone, so a failure here must
         * not abort the bridge -- we still want to re-open it below. */
        esp_err_t cerr = cdc_acm_host_close(event->data.cdc_hdl);
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "cdc_acm_host_close: %s", esp_err_to_name(cerr));
        }
        xSemaphoreGive(s_disconnected_sem);
        break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state notif 0x%04X", event->data.serial_state.val);
        break;
    case CDC_ACM_HOST_NETWORK_CONNECTION:
    default:
        ESP_LOGW(TAG, "Unsupported CDC event: %i", event->type);
        break;
    }
}

/* ----------------------------------------------------------- tasks */

/** USB Host library event loop. Required for any USB client on the chip. */
static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            /* Best-effort: a failure here must not kill the USB event task --
             * without it no USB recovery is possible at all. */
            esp_err_t ferr = usb_host_device_free_all();
            if (ferr != ESP_OK) {
                ESP_LOGW(TAG, "usb_host_device_free_all: %s", esp_err_to_name(ferr));
            }
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: All devices freed");
        }
    }
}

/** Open / configure / wait-for-disconnect loop. Re-opens automatically. */
static void usb_cdc_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = CDC_OPEN_TIMEOUT_MS,
        .out_buffer_size = CDC_BUF_SIZE,
        .in_buffer_size  = CDC_BUF_SIZE,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb  = handle_rx,
    };

    for (;;) {
        ESP_LOGD(TAG, "Opening CDC ACM device 0x%04X:0x%04X...",
                 CONFIG_USB_CDC_DEVICE_VID, CONFIG_USB_CDC_DEVICE_PID);
        /* Open + configure on a LOCAL handle the sink cannot see yet, so
         * line-coding / control-line setup can't race with a disconnect. */
        cdc_acm_dev_hdl_t new_dev = NULL;
        esp_err_t err = cdc_acm_host_open(CONFIG_USB_CDC_DEVICE_VID,
                                          CONFIG_USB_CDC_DEVICE_PID, 0,
                                          &dev_config, &new_dev);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "open failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(CDC_OPEN_RETRY_MS));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(CDC_OPEN_SETTLE_MS));

        /* Configure line coding: <baud> 8N1. These control transfers can
         * fail on a flaky/slow device right after open; on failure close the
         * half-opened handle and retry the whole open -- a reboot would defeat
         * the reconnect logic. */
        cdc_acm_line_coding_t line_coding = {
            .dwDTERate   = CONFIG_USB_CDC_BAUDRATE,
            .bDataBits   = 8,
            .bParityType = 0,
            .bCharFormat = 1,
        };
        esp_err_t cfg_err = cdc_acm_host_line_coding_set(new_dev, &line_coding);
        if (cfg_err == ESP_OK) {
            cfg_err = cdc_acm_host_set_control_line_state(new_dev, true, false);
        }
        if (cfg_err != ESP_OK) {
            ESP_LOGW(TAG, "device setup failed (%s); closing and retrying",
                     esp_err_to_name(cfg_err));
            cdc_acm_host_close(new_dev);   /* best effort */
            vTaskDelay(pdMS_TO_TICKS(CDC_OPEN_RETRY_MS));
            continue;
        }

        /* GET is informational only (log what the device actually applied);
         * a failure here is harmless and must not block publishing. */
        cdc_acm_line_coding_t applied;
        if (cdc_acm_host_line_coding_get(new_dev, &applied) == ESP_OK) {
            ESP_LOGI(TAG, "Line Get: Rate: %" PRIu32 ", Stop bits: %" PRIu8
                     ", Parity: %" PRIu8 ", Databits: %" PRIu8,
                     applied.dwDTERate, applied.bCharFormat,
                     applied.bParityType, applied.bDataBits);
        }

        /* Publish the fully-configured handle atomically: the sink sees either
         * NULL (skips) or a ready device -- never a half-configured one. */
        xSemaphoreTake(s_cdc_mutex, portMAX_DELAY);
        s_cdc_dev = new_dev;
        xSemaphoreGive(s_cdc_mutex);

        ESP_LOGI(TAG, "Connected CDC ACM device 0x%04X:0x%04X",
                 CONFIG_USB_CDC_DEVICE_VID, CONFIG_USB_CDC_DEVICE_PID);

        xSemaphoreTake(s_disconnected_sem, portMAX_DELAY);
    }
}

/* ----------------------------------------------------------- public API */

esp_err_t usb_cdc_init(void)
{
    s_disconnected_sem = xSemaphoreCreateBinary();
    s_cdc_mutex = xSemaphoreCreateMutex();
    if (s_disconnected_sem == NULL || s_cdc_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Installing USB Host");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    BaseType_t ok = xTaskCreate(usb_lib_task, "usb_lib", USB_LIB_TASK_STACK,
                                NULL, USB_HOST_PRIORITY, NULL);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create usb_lib task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Installing CDC-ACM driver");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    /* Self-register the TX sink, exactly like uart_bridge does. Must happen
     * before msg_router_start(). */
    esp_err_t ret = msg_router_register_sink(MSG_IF_USB_CDC, usb_cdc_sink);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register sink: %s", esp_err_to_name(ret));
        return ret;
    }

    ok = xTaskCreate(usb_cdc_task, "usb_cdc", USB_CDC_TASK_STACK,
                     NULL, USB_CDC_TASK_PRIO, NULL);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create usb_cdc task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
