# Tyt MD-9600 Многоинтерфейсный мост (USB CDC / BLE / UART)

[English](README.md) | **Русский**

| Целевая платформа | ESP32-S3 |
| ----------------- | -------- |

Прошивка для **ESP32-S3**, объединяющая несколько прозрачных последовательных
каналов через центральный маршрутизатор (`msg_router`):

- **USB CDC-ACM** устройство — радиостанция **Tyt MD-9600** (`1FC9:0094`);
- канал **Bluetooth Low Energy** через
  [Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html),
  до **4** одновременных клиентов;
- **UART Radio** — AT-устройство, скорость которого определяется
  **автоматически** (9600 / 115200);
- **UART Lora** — последовательный канал с фиксированной скоростью (по
  умолчанию 9600).

По умолчанию каждый кадр, принятый на одном интерфейсе, пересылается **во все
остальные**; индивидуальные правила маршрутизации по источнику задаются простым
`switch` внутри маршрутизатора (см. [Маршрутизация](#маршрутизация)).

> Целевая платформа — именно **ESP32-S3**: нужны одновременно USB-OTG в режиме
> host (для связи с рацией) **и** BLE-контроллер (у ESP32-S2 BLE отсутствует).

## Как это работает

```
                 ┌──────────────────── ESP32-S3 ────────────────────┐
   USB CDC-ACM ─►│                  msg_router                      │◄─ BLE NUS ─► клиенты (≤4)
   Tyt MD-9600   │              (задача-коммутатор)                 │   Nordic UART Service
   115200 8N1  ◄─┤                                                 │
                 │   по умолчанию: источник → ВСЕ остальные         │
   UART Radio ─►│   (переопределяется per-source `switch` в        │
   AT-устройство │    components/msg_router/src/msg_router.c)       │
   авто-бауд   ◄─┤   пробует 9600, затем 115200 по ответу на "AT"   │
                 │                                                 │
   UART Lora  ─►│                                                 │
   модуль        │                                                 │
   9600        ◄─┤   скорость из menuconfig (по умолчанию 9600)     │
                 └─────────────────────────────────────────────────┘
```

Четыре симметричных интерфейса, каждый является одновременно источником **и**
приёмником:

| Интерфейс | id в роутере | Физический канал |
|-----------|--------------|------------------|
| USB CDC   | `MSG_IF_USB_CDC`    | Tyt MD-9600 по USB CDC-ACM `1FC9:0094` @ 115200 8N1 |
| BLE NUS   | `MSG_IF_BLE`        | Nordic UART Service, до 4 клиентов |
| UART Radio| `MSG_IF_UART_RADIO` | UART-контроллер (по умолч. UART1), AT-устройство, авто-бауд 9600/115200 |
| UART Lora | `MSG_IF_UART_LORA`  | UART-контроллер (по умолч. UART2), фиксированная скорость (по умолч. 9600) |

| Направление | Поведение |
|-------------|-----------|
| Любой интерфейс → все остальные | Кадр, принятый на любом интерфейсе, по умолчанию доставляется **во все** sink'и. Каждый sink пропускает своего отправителя: одноканальные интерфейсы (USB, UART Radio, UART Lora) не возвращают кадр в ту же линию; **BLE пересылает всем клиентам, *кроме* отправителя** (`conn_handle` отправителя передаётся в кадре как origin и попадает в `nordic_uart_send_except()`). |

Мост **бинарно-прозрачный** (без построчной буферизации и трансляции CR-LF),
поэтому подходит как для CPS-протокола рации, так и для обычных AT-команд.

### Маршрутизация

Логика маршрутизации сосредоточена в одном месте: `switch` по источнику в
функции `route_message()`
([`components/msg_router/src/msg_router.c`](components/msg_router/src/msg_router.c)).
По умолчанию каждый `case` доставляет кадр всем sink'ам, а каждый sink исключает
своего отправителя (поэтому BLE-отправитель никогда не получает свой кадр
обратно — см. [Без обратной отправки отправителю BLE](#без-обратной-отправки-отправителю-ble)).
Чтобы ограничить источник конкретным набором адресатов, отредактируйте нужный
`case` и отправляйте явно через `msg_router_send_to()`:

```c
static void route_message(const msg_origin_t *origin,
                          const uint8_t *data, size_t len)
{
    switch (origin->iface) {
    case MSG_IF_UART_LORA:
        // Lora -> только в BLE
        msg_router_send_to(MSG_IF_BLE, origin, data, len);
        break;
    /* ...остальные case'ы сохраняют поведение hub по умолчанию... */
    }
}
```

#### Без обратной отправки отправителю BLE

Поскольку BLE многоканальный, BLE-кадр несёт `conn_handle` отправителя как
свой origin (`msg_origin_t.peer`). `main.c` помещает его через
`msg_submit_from()`:

```c
msg_origin_t origin = { .iface = MSG_IF_BLE, .peer = item->conn_handle };
msg_submit_from(&origin, item->data, item->len);
```

а BLE-sink исключает этого клиента:

```c
if (origin->iface == MSG_IF_BLE && origin->peer != MSG_PEER_NONE)
    nordic_uart_send_except(origin->peer, data, len);  // все, кроме отправителя
else
    nordic_uart_send(data, len);                       // broadcast (напр. ответ рации)
```

Таким образом, ввод клиента A доходит до рации, UART-каналов **и клиентов
B/C/D — но не до A**. Чтобы полностью изолировать BLE-клиентов друг от друга
(без фанаута между ними), уберите BLE-sink из `MSG_IF_BLE` case (см. комментарий
в `msg_router.c`).

### BLE-сервис

Стандартный **Nordic UART Service**:

| Элемент | UUID |
|---------|------|
| Сервис          | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (клиент → устройство, write)  | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (устройство → клиент, notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Рекламируемое имя устройства: **`DMR-RADIO`** (меняется в *Component config →
BLE Bridge* → `CONFIG_BLE_DEVICE_NAME`).

## Требуемое оборудование

- 1 × плата **ESP32-S3** с портом USB-OTG (напр. ESP32-S3-DevKitC, поддерживающая
  USB host).
- 1 × радиостанция **Tyt MD-9600** (представляется как USB CDC-ACM `1FC9:0094`),
  подключённая кабелем или через USB-A-брейкboard.
- (опционально) AT-модуль радиосвязи по UART.
- (опционально) **Lora**-модуль по UART.

### Подключение USB CDC (рация)

Соедините USB-линии от порта USB-OTG платы ESP32-S3 с рацией:

| ESP32-S3 (USB-OTG) | USB рации |
|--------------------|-----------|
| D+  (GPIO20)       | D+        |
| D-  (GPIO19)       | D-        |
| GND                | GND       |
| +5V (VBUS)         | +5V       |

> Обеспечьте достаточное питание. Рация должна быть запитана и включена (или
> находиться в режиме программирования), чтобы её CDC-интерфейс обнаружился.

### Подключение UART Radio (AT, авто-бауд)

| Вывод ESP32-S3 (по умолчанию) | AT-радиомодуль |
|-------------------------------|----------------|
| `CONFIG_UART_RADIO_TX_GPIO` (GPIO17) →  | RX  |
| `CONFIG_UART_RADIO_RX_GPIO` (GPIO18) ←  | TX  |
| GND                                      | GND |

Драйвер отправляет `AT` и ждёт ответ `OK`, перебирая сначала 9600, затем 115200.
Поиск повторяется, поэтому устройство можно подключить и после старта
(горячее подключение). Скрестите TX↔RX и объедините общую землю.

### Подключение UART Lora

| Вывод ESP32-S3 (по умолчанию) | Модуль Lora |
|-------------------------------|-------------|
| `CONFIG_UART_LORA_TX_GPIO` (GPIO21) →  | RX  |
| `CONFIG_UART_LORA_RX_GPIO` (GPIO47) ←  | TX  |
| GND                                      | GND |

> Все GPIO и номера UART-контроллеров настраиваются под вашу плату в `menuconfig`
> (см. [Конфигурация](#конфигурация)). UART-контроллеры должны различаться (по
> умолчанию: UART1 для Radio, UART2 для Lora).

## Конфигурация

Лимиты мульти-соединения BLE заданы в [`sdkconfig.defaults`](sdkconfig.defaults)
и зафиксированы в [`sdkconfig`](sdkconfig):

| Параметр | Значение | Назначение |
|----------|----------|------------|
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | `4` | Макс. одновременных BLE-клиентов |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | `8` | Активности BLE-контроллера (соединения + реклама) |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | `4` | Слоты bonding |
| `CONFIG_BT_NIMBLE_GATT_MAX_PROCS` | `8` | Одновременные GATT-процедуры |
| `CONFIG_NORDIC_UART_RX_BUFFER_SIZE` | `4096` | Размер кольцевого буфера BLE → байт |

Пины/скорости UART-интерфейсов и буфер роутера (*Component config → UART
Bridge* / *Message Router*):

| Параметр | По умолчанию | Назначение |
|----------|--------------|------------|
| `CONFIG_UART_RADIO_NUM`         | `1` (UART1) | UART-контроллер для AT-радио |
| `CONFIG_UART_RADIO_TX_GPIO`     | `17`        | TX → RX AT-радио |
| `CONFIG_UART_RADIO_RX_GPIO`     | `18`        | RX ← TX AT-радио |
| `CONFIG_UART_RADIO_BUF_SIZE`    | `512`       | Скретч-буфер Radio UART |
| `CONFIG_UART_RADIO_STARTUP_DELAY_MS` | `5000` | Задержка перед началом авто-бауда (мс) |
| `CONFIG_UART_RADIO_PROBE_ATTEMPTS`   | `3`    | Попыток авто-бауда, затем задача сдаётся |
| `CONFIG_UART_LORA_NUM`          | `2` (UART2) | UART-контроллер для Lora (должен отличаться от Radio) |
| `CONFIG_UART_LORA_TX_GPIO`      | `21`        | TX → RX Lora |
| `CONFIG_UART_LORA_RX_GPIO`      | `47`        | RX ← TX Lora |
| `CONFIG_UART_LORA_BAUDRATE`     | `9600`      | Фиксированная скорость Lora |
| `CONFIG_UART_LORA_BUF_SIZE`     | `512`       | Скретч-буфер Lora UART |
| `CONFIG_MSG_ROUTER_BUF_SIZE`         | `8192`      | Центральный буфер маршрутизации (байт) |
| `CONFIG_MSG_ROUTER_SINK_QUEUE_SIZE`  | `4096`      | Приватный буфер доставки per-sink (байт); drop-newest при переполнении |
| `CONFIG_MSG_ROUTER_SINK_TASK_STACK`  | `3072`      | Стек задачи доставки каждого sink'а |
| `CONFIG_MSG_ROUTER_STATS_PERIOD_MS` | `30000`     | Период лога счётчиков дропов (мс); `0` = выкл |

Параметры USB CDC задаются в *Component config → USB CDC* (`idf.py
menuconfig`): VID/PID рации (по умолч. `0x1FC9:0x0094`), скорость line coding
(по умолч. **115200 8N1**, DTR активен, RTS неактивен) и таймаут TX. Pinned-значения
также лежат в [`sdkconfig.defaults`](sdkconfig.defaults).

Чтобы изменить любое из этих значений, отредактируйте `sdkconfig.defaults` (или
выполните `idf.py menuconfig` → *Component config* → *Bluetooth* / *USB CDC* /
*UART Bridge* / *Message Router*) и пересоберите.

## Компоненты

Прошивка разбита на три локальных компонента в каталоге
[`components/`](components):

| Компонент | Роль |
|-----------|------|
| [`nordic_uart_multi`](components/nordic_uart_multi) | BLE-периферия Nordic UART Service с мульти-соединением (драйвер, не зависит от роутера) |
| [`ble_bridge`](components/ble_bridge)                  | Глю-слой BLE↔роутер: старт NUS + BLE-sink + задача `ble2rt` (NUS RX → роутер) |
| [`msg_router`](components/msg_router)              | Центральная очередь маршрутизации + задача `msg_routing` + асинхронные очереди per-sink + per-source `switch` |
| [`usb_cdc`](components/usb_cdc)                    | USB CDC-ACM (рация Tyt MD-9600): open/line-coding/авто-реконнект + TX-sink |
| [`uart_bridge`](components/uart_bridge)            | UART Radio (авто-бауд) + UART Lora (фикс. скорость) |

### `msg_router`

Асинхронная доставка per-sink (на каждый sink — своя приватная очередь **и**
своя задача доставки):

1. Производители помещают кадры в единый центральный кольцевой буфер
   (`msg_submit()` / `msg_submit_from()`).
2. Задача `msg_routing` выбирает кадры из буфера и для каждого кладёт копию в
   **приватную очередь** каждого зарегистрированного sink'а — неблокирующе и
   **с отбрасыванием при переполнении**. Политика маршрутизации — это `switch`
   по источнику в `route_message()` (см. [Маршрутизация](#маршрутизация)).
3. Каждый sink владеет собственной задачей доставки, которая разгребает свою
   очередь и вызывает реальную функцию доставки, которая может блокироваться
   свободно (USB tx, BLE notify, UART write) — страдает только этот sink, а
   роутер и остальные sink'и продолжают работать. Переполнившийся sink
   отбрасывает только свои новейшие кадры.

Регистрация sink'а через `msg_router_register_sink()` и создаёт его приватную
очередь, и запускает задачу доставки, поэтому все sink'и нужно регистрировать
**до** `msg_router_start()`.

```c
#include "msg_router.h"

#define MSG_PEER_NONE 0xFFFFu

typedef enum {
    MSG_IF_USB_CDC, MSG_IF_BLE, MSG_IF_UART_RADIO, MSG_IF_UART_LORA, MSG_IF_COUNT,
} msg_iface_t;

typedef struct {
    msg_iface_t iface;   /* интерфейс-источник                       */
    uint16_t    peer;    /* клиент-источник (BLE conn_handle) или MSG_PEER_NONE */
} msg_origin_t;

typedef void (*msg_sink_fn_t)(const msg_origin_t *origin, const uint8_t *data, size_t len);

esp_err_t msg_router_init(void);
esp_err_t msg_router_register_sink(msg_iface_t dest, msg_sink_fn_t fn); /* подключить интерфейс */
esp_err_t msg_submit      (msg_iface_t src, const uint8_t *data, size_t len);          /* одноканальный источник */
esp_err_t msg_submit_from (const msg_origin_t *origin, const uint8_t *data, size_t len); /* BLE (несёт отправителя) */
esp_err_t msg_router_start(void);                                                     /* запустить задачу */
void      msg_router_send_to(msg_iface_t dest, const msg_origin_t *origin,            /* явная пересылка */
                             const uint8_t *data, size_t len);
```

### `uart_bridge`

Обёртка над двумя UART-каналами. Каждый устанавливает UART-драйвер,
регистрирует свой sink в роутере и крутит RX-задачу, которая отправляет принятые
байты в роутер.

- **UART Radio** (`uart_radio_init()`): через `CONFIG_UART_RADIO_STARTUP_DELAY_MS`
  (по умолч. 5 с) после загрузки задача авто-бауда перебирает 9600/115200 по
  рукопожатию `AT`→`OK`, затем шлёт `AT+SYNCOVER` и ждёт `OK`; только после этого
  публикуется скорость и запускается RX-задача (бридж становится активен). До
  этого исходящие в Radio кадры отбрасываются (`b->baud` == 0). После
  `CONFIG_UART_RADIO_PROBE_ATTEMPTS` неудачных попыток задача останавливается —
  если рация не ответила за это время, бридж для неё остаётся выключенным.
- **UART Lora** (`uart_lora_init()`): фикс. скорость из конфига, RX-задача всегда
  активна.

### `nordic_uart_multi`

BLE-часть: периферия NUS с мульти-соединением и небольшим C-API.

```c
#include "nordic_uart_multi.h"

/* Жизненный цикл */
esp_err_t nordic_uart_start(const char *device_name);   /* NULL -> "Nordic UART" */
esp_err_t nordic_uart_stop(void);

/* Отправка (роутер -> BLE). Бинарно-безопасно, дробится по ATT MTU клиента. */
esp_err_t nordic_uart_send        (const uint8_t *data, size_t len);            /* всем клиентам       */
esp_err_t nordic_uart_send_to     (uint16_t conn_handle, const uint8_t *d, size_t len); /* одному     */
esp_err_t nordic_uart_send_except (uint16_t conn_handle, const uint8_t *d, size_t len); /* всем, кроме 1 */

/* Строковые варианты (NUL-terminated) для функций выше */
esp_err_t nordic_uart_send_str        (const char *str);
esp_err_t nordic_uart_send_str_to     (uint16_t conn_handle, const char *str);
esp_err_t nordic_uart_send_str_except (uint16_t conn_handle, const char *str);

/* Состояние */
uint8_t   nordic_uart_client_count(void);       /* подключённые клиенты      */
uint8_t   nordic_uart_subscribed_count(void);   /* клиенты с включенным TX notify */

/* Входящие данные (BLE -> роутер). Один кадр на запись клиента, с меткой источника. */
extern RingbufHandle_t nordic_uart_rx_buf_handle;

typedef struct {
    uint16_t conn_handle;   /* какой клиент отправил      */
    uint16_t len;           /* число валидных байт        */
    uint8_t  data[];        /* сырые данные               */
} nordic_uart_rx_item_t;
```

`main.c` выбирает данные из этого буфера и вызывает `msg_submit(MSG_IF_BLE, ...)`.

## Сборка и прошивка

Требуется **ESP-IDF v5.x** (разрабатывалось на v5.5.4).

```bash
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

Замените `PORT` на ваш последовательный порт (напр. `/dev/ttyUSB0`). Выход из
монитора — `Ctrl-]`.

## Ожидаемый вывод

После прошивки подключите рацию / UART-устройства и сопрягите BLE-клиент с
`DMR-RADIO`. Типичный лог:

```
I (xxx) MSG_ROUTER: Initialized (buffer 8192 bytes)
I (xxx) USB_CDC: Installing USB Host
I (xxx) USB_CDC: Installing CDC-ACM driver
I (xxx) UART_RADIO: Driver installed (UART1 TX=GPIO17 RX=GPIO18), auto-baud in 5000 ms...
I (xxx) UART_LORA:  Driver installed (UART2 TX=GPIO21 RX=GPIO47 @9600 bps)
I (xxx) NORDIC_UART: Started (max 4 connections)
I (xxx) MSG_ROUTER: Routing task started
I (xxx) USB_CDC: Opening CDC ACM device 0x1FC9:0x0094...
I (xxx) USB_CDC: Line Get: Rate: 115200, Stop bits: 1, Parity: 0, Databits: 8
I (xxx) USB_CDC: Connected CDC ACM device 0x1FC9:0x0094
I (xxx) NORDIC_UART: Connected handle=0  total=1/4
I (xxx) NORDIC_UART: Subscribe handle=0 notify=1
I (xxx) UART_RADIO: Startup delay 5000 ms before probing
I (xxx) UART_RADIO: AT OK at 115200 bps, sending init
I (xxx) UART_RADIO: Initialized @ 115200 bps, bridge active
I (xxx) UART_RADIO: RX task started (115200 bps)
I (xxx) USB_CDC: radio -> 5 byte(s)
I (xxx) BLE_BRIDGE: peer=0 -> 9 byte(s)
```

## Замечания / ограничения

- **Модель hub:** по умолчанию каждый принятый кадр пересылается во все
  остальные интерфейсы. Чтобы источник доставлял данные только конкретным
  адресатам, отредактируйте `switch` в
  [`msg_router.c`](components/msg_router/src/msg_router.c) (см.
  [Маршрутизация](#маршрутизация)).
- **Авто-бауд UART Radio:** через `CONFIG_UART_RADIO_STARTUP_DELAY_MS` после
  загрузки задача шлёт `AT` (определение 9600/115200), затем `AT+SYNCOVER`
  (инициализация); бридж активируется только после `OK` на обе команды. Пока
  инициализация не прошла, исходящие в Radio кадры отбрасываются. После
  `CONFIG_UART_RADIO_PROBE_ATTEMPTS` неудачных попыток задача сдаётся (рация не
  подключена/не отвечает) — повторного перебора скорости «на лету» нет.
- По умолчанию до **4** клиентов BLE (поднимите `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`
  для большего числа).
- USB-рация должна оставаться запитанной; при внезапном USB-разрыве прошивка
  автоматически переподключается к CDC-устройству.
- **Потери кадров наблюдаемы:** счётчики дропов (центральный буфер по источнику +
  per-sink очереди по адресату) логируются каждые `CONFIG_MSG_ROUTER_STATS_PERIOD_MS`
  (по умолч. 30 с), только если изменились; по запросу — `msg_router_log_stats()`.
- **USB-ошибки не роняют мост:** сбои line-coding / `cdc_acm_host_close` при
  подключении/отключении рации логируются и приводят к повтору open, а не к ребуту.
