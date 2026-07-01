/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Transparent UART bridge: one parameterized implementation serving both the
 * Radio link (AT device, auto-baud 9600/115200) and the Lora link (fixed
 * baud). The two public entry points uart_radio_init() / uart_lora_init() are
 * thin wrappers that supply a uart_bridge_cfg_t; everything else (sink, RX
 * task, driver install) is shared.
 *
 * The router sink API carries no user context, so each instance gets a tiny
 * sink trampoline (uart_radio_sink / uart_lora_sink) that closes over its
 * static uart_bridge_t.
 */
#include "uart_radio.h"
#include "uart_lora.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"

#include "msg_router.h"

/* ---- tasks ---- */
#define RX_TASK_STACK   4096
#define RX_TASK_PRIO    6
#define AB_TASK_STACK   4096
#define AB_TASK_PRIO    4
#define UART_QUEUE_LEN  20

/* ---- auto-baud (Radio only) ---- */
#define AT_CMD              "AT\r\n"
#define AT_INIT_CMD         "AT+SYNCOVER\r\n"
#define AT_RESP_TIMEOUT_MS  800    /**< how long to wait for an "OK" reply   */
#define PROBE_RETRY_DELAY   2000   /**< delay before re-probing when idle    */
static const uint32_t PROBE_BAUDS[] = {9600, 115200};
#define PROBE_BAUD_COUNT    (sizeof(PROBE_BAUDS) / sizeof(PROBE_BAUDS[0]))

/* --------------------------------------------------------------- types */

typedef struct {
    uart_port_t  port;
    int          tx_pin;
    int          rx_pin;
    int          buf_size;   /**< UART driver + RX scratch buffer size       */
    uint32_t     baud;       /**< fixed baud (Lora) or first probe (Radio)   */
    msg_iface_t  iface;      /**< router source/destination tag              */
    const char  *tag;        /**< log tag                                    */
    bool         auto_baud;  /**< Radio: true (probe AT->OK); Lora: false     */
} uart_bridge_cfg_t;

typedef struct {
    const uart_bridge_cfg_t *cfg;
    QueueHandle_t            evt_queue;
    volatile uint32_t        baud;  /**< 0 until the link is ready (Radio)  */
} uart_bridge_t;

/* The two instances. */
static uart_bridge_t s_radio;
static uart_bridge_t s_lora;

static void bridge_rx_task(void *arg);
static void bridge_autobaud_task(void *arg);

/* --------------------------------------------------------------- sink (TX) */

/** Core write: skip frames that originated on this same UART, drop them while
 *  the link isn't ready (Radio before auto-baud), otherwise push the bytes. */
static void bridge_write(uart_bridge_t *b, const msg_origin_t *origin,
                         const uint8_t *data, size_t len)
{
    if (origin->iface == b->cfg->iface) {
        return; /* don't echo back to our own UART */
    }
    if (b->baud == 0) {
        return; /* baud not detected yet: nowhere to send (Radio) */
    }
    uart_write_bytes(b->cfg->port, data, len);
}

/* Per-instance trampolines: the router stores a bare function pointer. */
static void uart_radio_sink(const msg_origin_t *o, const uint8_t *d, size_t l)
{
    bridge_write(&s_radio, o, d, l);
}

static void uart_lora_sink(const msg_origin_t *o, const uint8_t *d, size_t l)
{
    bridge_write(&s_lora, o, d, l);
}

/* --------------------------------------------------------------- RX task */

static void bridge_rx_task(void *arg)
{
    uart_bridge_t *b = (uart_bridge_t *)arg;
    const uart_bridge_cfg_t *cfg = b->cfg;

    ESP_LOGI(cfg->tag, "RX task started (%lu bps)", (unsigned long)b->baud);

    size_t buf_len = cfg->buf_size;
    uint8_t *buf = malloc(buf_len);
    if (buf == NULL) {
        ESP_LOGE(cfg->tag, "rx malloc fail");
        vTaskDelete(NULL);
        return;
    }

    uart_event_t event;
    for (;;) {
        if (!xQueueReceive(b->evt_queue, &event, portMAX_DELAY)) {
            continue;
        }
        if (event.type != UART_DATA || event.size == 0) {
            continue;
        }
        size_t want = event.size > buf_len ? buf_len : event.size;
        int n = uart_read_bytes(cfg->port, buf, want, portMAX_DELAY);
        if (n > 0) {
            msg_submit(cfg->iface, buf, (size_t)n);
        }
    }
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

/** Send an AT command (baud already set) and report whether the reply
 *  contains "OK". Flushes input and drains stale events first. */
static bool at_exchange_ok(uart_bridge_t *b, const char *cmd, size_t cmd_len,
                           uint8_t *scratch, size_t scratch_len)
{
    const uart_bridge_cfg_t *cfg = b->cfg;
    uart_flush_input(cfg->port);
    while (xQueueReceive(b->evt_queue, NULL, 0)) { }

    uart_write_bytes(cfg->port, cmd, cmd_len);
    uart_wait_tx_done(cfg->port, pdMS_TO_TICKS(100));

    int n = uart_read_bytes(cfg->port, scratch, scratch_len - 1,
                            pdMS_TO_TICKS(AT_RESP_TIMEOUT_MS));
    if (n <= 0) {
        return false;
    }
    scratch[n] = '\0';
    return resp_contains_ok(scratch, n);
}

/** Set @p baud, let the line coding settle, then run the AT handshake. */
static bool probe_baud(uart_bridge_t *b, uint32_t baud,
                       uint8_t *scratch, size_t scratch_len)
{
    if (uart_set_baudrate(b->cfg->port, baud) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));   /* let the new line coding settle */
    return at_exchange_ok(b, AT_CMD, sizeof(AT_CMD) - 1, scratch, scratch_len);
}

/**
 * Radio bring-up, run in its own task:
 *   1. wait CONFIG_UART_RADIO_STARTUP_DELAY_MS after boot;
 *   2. probe 9600/115200 by sending "AT" and looking for "OK" (baud detect);
 *   3. on a match, send the "AT+SYNCOVER" init command and expect "OK";
 *   4. only then publish the baud and start the RX task (bridge goes live);
 *   5. give up after CONFIG_UART_RADIO_PROBE_ATTEMPTS rounds -- if the radio
 *      hasn't answered by then it probably isn't connected, so stop poking
 *      the line (the bridge stays disabled; b->baud stays 0 -> frames dropped).
 */
static void bridge_autobaud_task(void *arg)
{
    uart_bridge_t *b = (uart_bridge_t *)arg;
    const uart_bridge_cfg_t *cfg = b->cfg;

    size_t scratch_len = cfg->buf_size;
    uint8_t *scratch = malloc(scratch_len);
    if (scratch == NULL) {
        ESP_LOGE(cfg->tag, "autobaud malloc fail");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(cfg->tag, "Startup delay %d ms before probing",
             CONFIG_UART_RADIO_STARTUP_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_UART_RADIO_STARTUP_DELAY_MS));

    for (int attempt = 1; attempt <= CONFIG_UART_RADIO_PROBE_ATTEMPTS; attempt++) {
        for (size_t i = 0; i < PROBE_BAUD_COUNT; i++) {
            if (!probe_baud(b, PROBE_BAUDS[i], scratch, scratch_len)) {
                ESP_LOGD(cfg->tag, "No AT reply at %lu bps",
                         (unsigned long)PROBE_BAUDS[i]);
                continue;
            }

            /* Baud confirmed via AT. Run the init command at this baud. */
            b->baud = PROBE_BAUDS[i];
            ESP_LOGI(cfg->tag, "AT OK at %lu bps, sending init",
                     (unsigned long)b->baud);
            if (at_exchange_ok(b, AT_INIT_CMD, sizeof(AT_INIT_CMD) - 1,
                               scratch, scratch_len)) {
                uart_flush_input(cfg->port);
                while (xQueueReceive(b->evt_queue, NULL, 0)) { }
                ESP_LOGI(cfg->tag, "Initialized @ %lu bps, bridge active",
                         (unsigned long)b->baud);
                /* RX task starts only now, after probing+init are done and the
                 * link is flushed, so it never races with the reads above. */
                xTaskCreate(bridge_rx_task, "uart_radio_rx",
                            RX_TASK_STACK, b, RX_TASK_PRIO, NULL);
                free(scratch);
                vTaskDelete(NULL);
                return;
            }
            ESP_LOGW(cfg->tag, "AT+SYNCOVER failed at %lu bps",
                     (unsigned long)b->baud);
            b->baud = 0;   /* not ready: keep dropping frames */
            break;         /* AT matched this baud; no point trying the other */
        }
        if (attempt < CONFIG_UART_RADIO_PROBE_ATTEMPTS) {
            ESP_LOGI(cfg->tag, "No radio (attempt %d/%d), retry in %d ms",
                     attempt, CONFIG_UART_RADIO_PROBE_ATTEMPTS, PROBE_RETRY_DELAY);
            vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_DELAY));
        }
    }

    ESP_LOGW(cfg->tag, "No AT radio after %d attempt(s); bridge disabled",
             CONFIG_UART_RADIO_PROBE_ATTEMPTS);
    free(scratch);
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------- install */

static esp_err_t bridge_install(uart_bridge_t *b, const uart_bridge_cfg_t *cfg,
                                msg_sink_fn_t sink)
{
    b->cfg = cfg;
    b->baud = cfg->auto_baud ? 0 : cfg->baud;

    const uart_config_t ucfg = {
        .baud_rate  = cfg->baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(cfg->port,
                                        cfg->buf_size * 2,
                                        cfg->buf_size * 2,
                                        UART_QUEUE_LEN, &b->evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->port, &ucfg));
    ESP_ERROR_CHECK(uart_set_pin(cfg->port,
                                 cfg->tx_pin, cfg->rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    msg_router_register_sink(cfg->iface, sink);
    return ESP_OK;
}

/* --------------------------------------------------------------- public API */

esp_err_t uart_radio_init(void)
{
    static const uart_bridge_cfg_t cfg = {
        .port      = (uart_port_t)CONFIG_UART_RADIO_NUM,
        .tx_pin    = CONFIG_UART_RADIO_TX_GPIO,
        .rx_pin    = CONFIG_UART_RADIO_RX_GPIO,
        .buf_size  = CONFIG_UART_RADIO_BUF_SIZE,
        .baud      = PROBE_BAUDS[0], /* changed later by auto-baud */
        .iface     = MSG_IF_UART_RADIO,
        .tag       = "UART_RADIO",
        .auto_baud = true,
    };

    ESP_ERROR_CHECK(bridge_install(&s_radio, &cfg, uart_radio_sink));

    /* RX task is launched by the auto-baud task once a baud is confirmed. */
    xTaskCreate(bridge_autobaud_task, "uart_radio_ab", AB_TASK_STACK,
                &s_radio, AB_TASK_PRIO, NULL);

    ESP_LOGI(cfg.tag, "Driver installed (UART%d TX=GPIO%d RX=GPIO%d), "
             "auto-baud in %d ms...",
             cfg.port, cfg.tx_pin, cfg.rx_pin, CONFIG_UART_RADIO_STARTUP_DELAY_MS);
    return ESP_OK;
}

esp_err_t uart_lora_init(void)
{
    static const uart_bridge_cfg_t cfg = {
        .port      = (uart_port_t)CONFIG_UART_LORA_NUM,
        .tx_pin    = CONFIG_UART_LORA_TX_GPIO,
        .rx_pin    = CONFIG_UART_LORA_RX_GPIO,
        .buf_size  = CONFIG_UART_LORA_BUF_SIZE,
        .baud      = CONFIG_UART_LORA_BAUDRATE,
        .iface     = MSG_IF_UART_LORA,
        .tag       = "UART_LORA",
        .auto_baud = false,
    };

    ESP_ERROR_CHECK(bridge_install(&s_lora, &cfg, uart_lora_sink));

    xTaskCreate(bridge_rx_task, "uart_lora_rx", RX_TASK_STACK,
                &s_lora, RX_TASK_PRIO, NULL);

    ESP_LOGI(cfg.tag, "Driver installed (UART%d TX=GPIO%d RX=GPIO%d @%d bps)",
             cfg.port, cfg.tx_pin, cfg.rx_pin, CONFIG_UART_LORA_BAUDRATE);
    return ESP_OK;
}
