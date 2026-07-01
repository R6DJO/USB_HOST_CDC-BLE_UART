# Tyt MD-9600 BLE-UART Bridge (multi-connection)

**English** | [Русский](README.ru.md)

| Supported Target | ESP32-S3 |
| ---------------- | -------- |

A firmware for the **ESP32-S3** that bridges a USB CDC-ACM device (the
**Tyt MD-9600** DMR radio) and a **Bluetooth Low Energy** link, using the
[Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html).

It lets one or more (up to **4**) BLE clients — a phone, a PC, a tablet — talk
to the radio wirelessly over a serial-like channel, instead of a USB cable.

> The target is **ESP32-S3** specifically: it needs both a USB-OTG host port
> (to talk to the radio) **and** a BLE controller (the ESP32-S2 has no BLE).

## How it works

```
   ┌─────────────┐    USB CDC-ACM     ┌──────────────────┐    BLE NUS      ┌──────────┐
   │  Tyt MD-9600│  ◄──────────────►  │   ESP32-S3       │  ◄──────────►  │  Client 1│
   │  (radio)    │   0x1FC9:0x0094    │  (this firmware) │                 │  Client 2│
   │             │    115200 8N1      │                  │                 │  ... up  │
   └─────────────┘                    └──────────────────┘                 │  to 4    │
       VID:PID = 1FC9:0094                                                  └──────────┘
```

| Direction | Behaviour |
|-----------|-----------|
| **Radio → BLE** (`UART->BLE`) | Data received from the radio over USB is **broadcast to every** subscribed BLE peer. |
| **BLE → Radio** (`BLE->UART`) | Data written by **any** peer is forwarded to the radio over USB. Each received frame is tagged with its source `conn_handle`. |

The bridge is **binary-transparent** (no line buffering / CR-LF translation), so
it is suitable for the radio's CPS protocol as well as plain AT commands.

### BLE service

Standard **Nordic UART Service**:

| Element | UUID |
|---------|------|
| Service        | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (peer → us, write)  | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (us → peer, notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Advertised device name: **`DMR-RADIO`**.

## Hardware required

- 1 × **ESP32-S3** board with a USB-OTG port (e.g. ESP32-S3-DevKitC, USB host
  capable).
- 1 × **Tyt MD-9600** radio (presents itself as USB CDC-ACM `1FC9:0094`),
  connected by cable or via a USB-A breakout.

Connect the USB signals from the ESP32-S3 OTG port to the radio:

| ESP32-S3 (USB-OTG) | Radio USB |
|--------------------|-----------|
| D+  (GPIO20)       | D+        |
| D-  (GPIO19)       | D-        |
| GND                | GND       |
| +5V (VBUS)         | +5V       |

> Provide adequate power. The radio must be powered and switched on (or in
> programming mode) for its CDC interface to enumerate.

## Configuration

BLE multi-connection limits live in [`sdkconfig.defaults`](sdkconfig.defaults)
and are baked into [`sdkconfig`](sdkconfig):

| Option | Value | Meaning |
|--------|-------|---------|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | `4` | Max simultaneous BLE peers |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | `8` | Controller BLE activities (conns + adv) |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | `4` | Bonding slots |
| `CONFIG_BT_NIMBLE_GATT_MAX_PROCS` | `8` | Concurrent GATT procedures |
| `CONFIG_NORDIC_UART_RX_BUFFER_SIZE` | `4096` | BLE → ring buffer size (bytes) |

USB CDC parameters are set in [`main/main.c`](main/main.c): **115200 8N1**, DTR
asserted, RTS deasserted. Adjust `MD9600_USB_DEVICE_VID` / `PID` there if your
radio reports different IDs.

To change the number of allowed peers, edit `sdkconfig.defaults` (or run
`idf.py menuconfig` → *Component config* → *Bluetooth* → *NimBLE options* →
*Maximum number of concurrent connections*) and rebuild.

## The `nordic_uart_multi` component

The BLE side is a local component: [`components/nordic_uart_multi`](components/nordic_uart_multi).
It implements the multi-connection NUS peripheral and exposes a small C API:

```c
#include "nordic_uart_multi.h"

/* Lifecycle */
esp_err_t nordic_uart_start(const char *device_name);   /* NULL name -> "Nordic UART" */
esp_err_t nordic_uart_stop(void);

/* Sending (radio -> BLE). Binary-safe, chunked per peer's ATT MTU. */
esp_err_t nordic_uart_send        (const uint8_t *data, size_t len);            /* all peers         */
esp_err_t nordic_uart_send_to     (uint16_t conn_handle, const uint8_t *d, size_t len); /* one peer  */
esp_err_t nordic_uart_send_except (uint16_t conn_handle, const uint8_t *d, size_t len); /* all but 1 */

/* Convenience NUL-terminated-string variants of the above */
esp_err_t nordic_uart_send_str        (const char *str);
esp_err_t nordic_uart_send_str_to     (uint16_t conn_handle, const char *str);
esp_err_t nordic_uart_send_str_except (uint16_t conn_handle, const char *str);

/* Status */
uint8_t   nordic_uart_client_count(void);       /* connected peers   */
uint8_t   nordic_uart_subscribed_count(void);   /* peers with TX notify on */

/* Incoming data (BLE -> radio). One frame per peer write, tagged with source. */
extern RingbufHandle_t nordic_uart_rx_buf_handle;

typedef struct {
    uint16_t conn_handle;   /* which peer sent this        */
    uint16_t len;           /* number of valid bytes       */
    uint8_t  data[];        /* raw payload                 */
} nordic_uart_rx_item_t;
```

Drain the RX buffer from your task:

```c
size_t sz = 0;
const nordic_uart_rx_item_t *it = xRingbufferReceive(nordic_uart_rx_buf_handle, &sz, portMAX_DELAY);
if (it) {
    /* forward it->data (it->len bytes) to the radio; it->conn_handle is the source */
    vRingbufferReturnItem(nordic_uart_rx_buf_handle, (void *)it);
}
```

## Build and flash

Requires **ESP-IDF v5.x** (developed against v5.5.4).

```bash
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (e.g. `/dev/ttyUSB0`). Exit the monitor
with `Ctrl-]`.

## Expected output

After flashing, connect the radio and pair a BLE client to `DMR-RADIO`. Typical
log:

```
I (xxx) DMR-RADIO: Installing USB Host
I (xxx) DMR-RADIO: Installing CDC-ACM driver
I (xxx) NORDIC_UART: Started (max 4 connections)
I (xxx) DMR-RADIO: Opening CDC ACM device 0x1FC9:0x0094...
I (xxx) DMR-RADIO: Line Get: Rate: 115200, Stop bits: 1, Parity: 0, Databits: 8
I (xxx) DMR-RADIO: Connected CDC ACM device 0x1FC9:0x0094 (0 BLE peer(s))...
I (xxx) NORDIC_UART: Connected handle=0  total=1/4
I (xxx) NORDIC_UART: Subscribe handle=0 notify=1
I (xxx) UART->BLE: radio -> 5 byte(s) to 1 peer(s)
I (xxx) BLE->UART: peer=0 -> radio 9 byte(s)
```

## Notes / limitations

- **Broadcast model:** the radio is a single USB sink, so the firmware broadcasts
  radio replies to all subscribed peers and accepts input from any of them. Use
  `nordic_uart_send_to()` / `nordic_uart_send_except()` from your application
  code if you need point-to-point or "all-but-the-sender" delivery.
- Up to **4** peers by default (ESP32-S3 controller/stack limit is higher; raise
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` if you need more).
- The radio must stay powered; on sudden USB disconnect the firmware reopens the
  CDC device automatically.
