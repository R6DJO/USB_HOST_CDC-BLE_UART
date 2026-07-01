# Tyt MD-9600 Multi-Interface Bridge (USB CDC / BLE / UART)

**English** | [Русский](README.ru.md)

| Supported Target | ESP32-S3 |
| ---------------- | -------- |

A firmware for the **ESP32-S3** that bridges several transparent serial
channels through a central message router (`msg_router`):

- a **USB CDC-ACM** device — the **Tyt MD-9600** DMR radio (`1FC9:0094`);
- a **Bluetooth Low Energy** link using the
  [Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html),
  up to **4** simultaneous peers;
- a **UART Radio** — an AT device whose baud is **auto-detected** (9600 / 115200);
- a **UART Lora** — a fixed-baud serial link (default 9600).

By default every frame received on one interface is forwarded to **all the
others**; per-source routing rules can be set with a simple `switch` inside the
router (see [Routing](#routing)).

> The target is **ESP32-S3** specifically: it needs both a USB-OTG host port
> (to talk to the radio) **and** a BLE controller (the ESP32-S2 has no BLE).

## How it works

```
                 ┌──────────────────── ESP32-S3 ────────────────────┐
   USB CDC-ACM ─►│                  msg_router                      │◄─ BLE NUS ─► peers (≤4)
   Tyt MD-9600   │               (hub / switch task)                │   Nordic UART Service
   115200 8N1  ◄─┤                                                 │
                 │   default: each source → ALL other destinations  │
   UART Radio ─►│   (override per source in the `switch` in        │
   AT device     │    components/msg_router/src/msg_router.c)       │
   auto-baud   ◄─┤   probes 9600 then 115200 via an "AT" reply      │
                 │                                                 │
   UART Lora  ─►│                                                 │
   module        │                                                 │
   9600        ◄─┤   baud from menuconfig (default 9600)           │
                 └─────────────────────────────────────────────────┘
```

Four symmetric interfaces, each a source **and** a destination:

| Interface | Router id | Physical link |
|-----------|-----------|---------------|
| USB CDC   | `MSG_IF_USB_CDC`    | Tyt MD-9600 over USB CDC-ACM `1FC9:0094` @ 115200 8N1 |
| BLE NUS   | `MSG_IF_BLE`        | Nordic UART Service, up to 4 peers |
| UART Radio| `MSG_IF_UART_RADIO` | UART controller (default UART1), AT device, auto-baud 9600/115200 |
| UART Lora | `MSG_IF_UART_LORA`  | UART controller (default UART2), fixed baud (default 9600) |

| Direction | Behaviour |
|-----------|-----------|
| Any interface → all others | A frame received on any interface is, by default, forwarded to **every other** interface (hub / broadcast). Each interface tags its frames with its router id. |

The bridge is **binary-transparent** (no line buffering / CR-LF translation), so
it is suitable for the radio's CPS protocol as well as plain AT commands.

### Routing

Routing lives in a single place: the `switch` over the source id in
`route_message()` ([`components/msg_router/src/msg_router.c`](components/msg_router/src/msg_router.c)).
The default body of every case forwards to all other interfaces. To restrict a
source to a specific set of destinations, edit the matching case and forward
explicitly with `msg_router_send_to()`:

```c
static void route_message(msg_iface_t src, const uint8_t *data, size_t len)
{
    switch (src) {
    case MSG_IF_UART_LORA:
        // Lora -> BLE only
        msg_router_send_to(MSG_IF_BLE, data, len);
        break;
    /* ...other cases keep the default hub behaviour... */
    }
}
```

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
- (optional) an AT-commands serial **radio** module on a UART.
- (optional) a **Lora** module on a UART.

### USB CDC (radio) wiring

Connect the USB signals from the ESP32-S3 OTG port to the radio:

| ESP32-S3 (USB-OTG) | Radio USB |
|--------------------|-----------|
| D+  (GPIO20)       | D+        |
| D-  (GPIO19)       | D-        |
| GND                | GND       |
| +5V (VBUS)         | +5V       |

> Provide adequate power. The radio must be powered and switched on (or in
> programming mode) for its CDC interface to enumerate.

### UART Radio (AT, auto-baud) wiring

| ESP32-S3 pin (default) | AT radio module |
|------------------------|-----------------|
| `CONFIG_UART_RADIO_TX_GPIO` (GPIO17) →  | RX  |
| `CONFIG_UART_RADIO_RX_GPIO` (GPIO18) ←  | TX  |
| GND                                      | GND |

The driver sends `AT` and listens for an `OK` reply, trying 9600 first and then
115200. It keeps retrying, so the device may be connected after boot
(hot-plug). Cross connect TX↔RX and share a common ground.

### UART Lora wiring

| ESP32-S3 pin (default) | Lora module |
|------------------------|-------------|
| `CONFIG_UART_LORA_TX_GPIO` (GPIO21) →  | RX  |
| `CONFIG_UART_LORA_RX_GPIO` (GPIO47) ←  | TX  |
| GND                                      | GND |

> Adjust every GPIO and the UART peripheral number to your board in
> `menuconfig` (see [Configuration](#configuration)). The two UART peripherals
> must be different (defaults: UART1 for Radio, UART2 for Lora).

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

UART interface pins/rates and the router buffer (*Component config → UART
Bridge* / *Message Router*):

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_UART_RADIO_NUM`         | `1` (UART1) | UART peripheral for the AT radio |
| `CONFIG_UART_RADIO_TX_GPIO`     | `17`        | TX → AT radio RX |
| `CONFIG_UART_RADIO_RX_GPIO`     | `18`        | RX ← AT radio TX |
| `CONFIG_UART_RADIO_BUF_SIZE`    | `512`       | Radio UART scratch buffer |
| `CONFIG_UART_LORA_NUM`          | `2` (UART2) | UART peripheral for Lora (must differ from Radio) |
| `CONFIG_UART_LORA_TX_GPIO`      | `21`        | TX → Lora RX |
| `CONFIG_UART_LORA_RX_GPIO`      | `47`        | RX ← Lora TX |
| `CONFIG_UART_LORA_BAUDRATE`     | `9600`      | Fixed Lora baud rate |
| `CONFIG_UART_LORA_BUF_SIZE`     | `512`       | Lora UART scratch buffer |
| `CONFIG_MSG_ROUTER_BUF_SIZE`    | `8192`      | Central routing ring buffer (bytes) |

USB CDC parameters are set in [`main/main.c`](main/main.c): **115200 8N1**, DTR
asserted, RTS deasserted. Adjust `MD9600_USB_DEVICE_VID` / `PID` there if your
radio reports different IDs.

To change any of the above, edit `sdkconfig.defaults` (or run `idf.py menuconfig`
→ *Component config* → *Bluetooth* / *UART Bridge* / *Message Router*) and
rebuild.

## Components

The firmware is split into three local components under
[`components/`](components):

| Component | Role |
|-----------|------|
| [`nordic_uart_multi`](components/nordic_uart_multi) | Multi-connection BLE Nordic UART Service peripheral |
| [`msg_router`](components/msg_router)              | Central routing queue + `msg_routing` task + per-source `switch` |
| [`uart_bridge`](components/uart_bridge)            | UART Radio (auto-baud) + UART Lora (fixed baud) |

### `msg_router`

A single FreeRTOS ring buffer queues frames tagged with their source; one
`msg_routing` task drains it and dispatches each frame to destination
"sinks" registered with `msg_router_register_sink()`. Producers push frames
with `msg_submit()`. The routing policy is the `switch` described in
[Routing](#routing).

```c
#include "msg_router.h"

typedef enum {
    MSG_IF_USB_CDC, MSG_IF_BLE, MSG_IF_UART_RADIO, MSG_IF_UART_LORA, MSG_IF_COUNT,
} msg_iface_t;

typedef void (*msg_sink_fn_t)(const uint8_t *data, size_t len);

esp_err_t msg_router_init(void);
esp_err_t msg_router_register_sink(msg_iface_t dest, msg_sink_fn_t fn); /* plug an interface in */
esp_err_t msg_submit(msg_iface_t src, const uint8_t *data, size_t len);  /* producer entry point */
esp_err_t msg_router_start(void);                                        /* spawn the task */
void      msg_router_send_to(msg_iface_t dest, const uint8_t *data, size_t len); /* explicit forward */
```

### `uart_bridge`

Wraps the two UART links. Each one installs the UART driver, registers its sink
with the router, and runs an RX task that forwards received bytes into the
router.

- **UART Radio** (`uart_radio_init()`): auto-baud task probes 9600/115200 by an
  `AT`→`OK` handshake; once a baud is found it spawns the RX task. Before that,
  outbound frames are dropped (`uart_radio_baud()` returns 0).
- **UART Lora** (`uart_lora_init()`): fixed baud from config, RX task always on.

### `nordic_uart_multi`

The BLE side: a multi-connection NUS peripheral exposing a small C API.

```c
#include "nordic_uart_multi.h"

/* Lifecycle */
esp_err_t nordic_uart_start(const char *device_name);   /* NULL name -> "Nordic UART" */
esp_err_t nordic_uart_stop(void);

/* Sending (router -> BLE). Binary-safe, chunked per peer's ATT MTU. */
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

/* Incoming data (BLE -> router). One frame per peer write, tagged with source. */
extern RingbufHandle_t nordic_uart_rx_buf_handle;

typedef struct {
    uint16_t conn_handle;   /* which peer sent this        */
    uint16_t len;           /* number of valid bytes       */
    uint8_t  data[];        /* raw payload                 */
} nordic_uart_rx_item_t;
```

`main.c` drains that buffer and calls `msg_submit(MSG_IF_BLE, ...)`.

## Build and flash

Requires **ESP-IDF v5.x** (developed against v5.5.4).

```bash
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (e.g. `/dev/ttyUSB0`). Exit the monitor
with `Ctrl-]`.

## Expected output

After flashing, connect the radio / UART devices and pair a BLE client to
`DMR-RADIO`. Typical log:

```
I (xxx) MSG_ROUTER: Initialized (buffer 8192 bytes)
I (xxx) UART_RADIO: Driver installed (UART1 TX=GPIO17 RX=GPIO18), probing baud...
I (xxx) UART_LORA:  Driver installed (UART2 TX=GPIO21 RX=GPIO47 @9600 bps)
I (xxx) MSG_ROUTER: Routing task started
I (xxx) NORDIC_UART: Started (max 4 connections)
I (xxx) DMR-RADIO: Installing USB Host
I (xxx) DMR-RADIO: Installing CDC-ACM driver
I (xxx) UART_RADIO: Baud detected: 115200 bps
I (xxx) UART_RADIO: RX task started (115200 bps)
I (xxx) DMR-RADIO: Opening CDC ACM device 0x1FC9:0x0094...
I (xxx) DMR-RADIO: Line Get: Rate: 115200, Stop bits: 1, Parity: 0, Databits: 8
I (xxx) DMR-RADIO: Connected CDC ACM device 0x1FC9:0x0094 (0 BLE peer(s))...
I (xxx) NORDIC_UART: Connected handle=0  total=1/4
I (xxx) NORDIC_UART: Subscribe handle=0 notify=1
I (xxx) USB->ROUTER:  radio -> 5 byte(s)
I (xxx) BLE->ROUTER:  peer=0 -> 9 byte(s)
```

## Notes / limitations

- **Hub model:** by default every received frame is forwarded to all other
  interfaces. To make a source deliver only to specific destinations, edit the
  `switch` in [`msg_router.c`](components/msg_router/src/msg_router.c) (see
  [Routing](#routing)).
- **UART Radio auto-baud:** until an `AT`→`OK` handshake succeeds, the Radio
  UART only listens and outbound frames to it are dropped. Re-detection is
  automatic on hot-plug.
- Up to **4** BLE peers by default (raise `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` for
  more).
- The USB radio must stay powered; on sudden USB disconnect the firmware reopens
  the CDC device automatically.
