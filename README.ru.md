# Tyt MD-9600 BLE-UART мост (мульти-соединение)

[English](README.md) | **Русский**

| Целевая платформа | ESP32-S3 |
| ----------------- | -------- |

Прошивка для **ESP32-S3**, выполняющая роль моста между USB CDC-ACM устройством
(рации **Tyt MD-9600**, DMR) и каналом **Bluetooth Low Energy**, через
[Nordic UART Service (NUS)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/libraries/bluetooth_services/services/nus.html).

Прошивка позволяет одному или нескольким (до **4**) BLE-клиентам — телефону, ПК,
планшету — общаться с радиостанцией по беспроводному последовательному каналу
вместо USB-кабеля.

> Целевая платформа — именно **ESP32-S3**: нужны одновременно USB-OTG в режиме
> host (для связи с рацией) **и** BLE-контроллер (у ESP32-S2 BLE отсутствует).

## Как это работает

```
   ┌─────────────┐    USB CDC-ACM     ┌──────────────────┐    BLE NUS      ┌──────────┐
   │  Tyt MD-9600│  ◄──────────────►  │   ESP32-S3       │  ◄──────────►  │  Клиент 1│
   │  (рация)    │   0x1FC9:0x0094    │  (эта прошивка)  │                │  Клиент 2│
   │             │    115200 8N1      │                  │                │  ... до  │
   └─────────────┘                    └──────────────────┘                │   4      │
       VID:PID = 1FC9:0094                                                 └──────────┘
```

| Направление | Поведение |
|-------------|-----------|
| **Рация → BLE** (`UART->BLE`) | Данные, принятые от рации по USB, **рассылаются всем** подписанным BLE-клиентам (broadcast). |
| **BLE → Рация** (`BLE->UART`) | Данные, записанные **любым** клиентом, пересылаются в рацию по USB. Каждый принятый кадр помечается источником `conn_handle`. |

Мост **бинарно-прозрачный** (без построчной буферизации и трансляции CR-LF),
поэтому подходит как для CPS-протокола рации, так и для обычных AT-команд.

### BLE-сервис

Стандартный **Nordic UART Service**:

| Элемент | UUID |
|---------|------|
| Сервис          | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (клиент → устройство, write)  | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (устройство → клиент, notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Рекламируемое имя устройства: **`DMR-RADIO`**.

## Требуемое оборудование

- 1 × плата **ESP32-S3** с портом USB-OTG (напр. ESP32-S3-DevKitC, поддерживающая
  USB host).
- 1 × радиостанция **Tyt MD-9600** (представляется как USB CDC-ACM `1FC9:0094`),
  подключённая кабелем или через USB-A-брейкboard.

Соедините USB-линии от порта USB-OTG платы ESP32-S3 с рацией:

| ESP32-S3 (USB-OTG) | USB рации |
|--------------------|-----------|
| D+  (GPIO20)       | D+        |
| D-  (GPIO19)       | D-        |
| GND                | GND       |
| +5V (VBUS)         | +5V       |

> Обеспечьте достаточное питание. Рация должна быть запитана и включена (или
> находиться в режиме программирования), чтобы её CDC-интерфейс обнаружился.

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

Параметры USB CDC задаются в [`main/main.c`](main/main.c): **115200 8N1**, DTR
активен, RTS неактивен. При необходимости поменяйте `MD9600_USB_DEVICE_VID` /
`PID` там же, если рация сообщает другие ID.

Чтобы изменить допустимое число клиентов, отредактируйте `sdkconfig.defaults`
(или выполните `idf.py menuconfig` → *Component config* → *Bluetooth* →
*NimBLE options* → *Maximum number of concurrent connections*) и пересоберите.

## Компонент `nordic_uart_multi`

BLE-часть реализована локальным компонентом:
[`components/nordic_uart_multi`](components/nordic_uart_multi). Он реализует
периферию NUS с мульти-соединением и предоставляет небольшой C-API:

```c
#include "nordic_uart_multi.h"

/* Жизненный цикл */
esp_err_t nordic_uart_start(const char *device_name);   /* NULL -> "Nordic UART" */
esp_err_t nordic_uart_stop(void);

/* Отправка (рация -> BLE). Бинарно-безопасно, дробится по ATT MTU клиента. */
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

/* Входящие данные (BLE -> рация). Один кадр на запись клиента, с меткой источника. */
extern RingbufHandle_t nordic_uart_rx_buf_handle;

typedef struct {
    uint16_t conn_handle;   /* какой клиент отправил      */
    uint16_t len;           /* число валидных байт        */
    uint8_t  data[];        /* сырые данные               */
} nordic_uart_rx_item_t;
```

Чтение из RX-буфера в вашей задаче:

```c
size_t sz = 0;
const nordic_uart_rx_item_t *it = xRingbufferReceive(nordic_uart_rx_buf_handle, &sz, portMAX_DELAY);
if (it) {
    /* переслать it->data (it->len байт) в рацию; it->conn_handle — источник */
    vRingbufferReturnItem(nordic_uart_rx_buf_handle, (void *)it);
}
```

## Сборка и прошивка

Требуется **ESP-IDF v5.x** (разрабатывалось на v5.5.4).

```bash
idf.py set-target esp32s3
idf.py -p PORT flash monitor
```

Замените `PORT` на ваш последовательный порт (напр. `/dev/ttyUSB0`). Выход из
монитора — `Ctrl-]`.

## Ожидаемый вывод

После прошивки подключите рацию и сопрягите BLE-клиент с `DMR-RADIO`. Типичный
лог:

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

## Замечания / ограничения

- **Broadcast-модель:** рация — единый приёмник по USB, поэтому ответы рации
  рассылаются всем подписанным клиентам, а ввод принимается от любого из них.
  При необходимости точечной или «всем, кроме отправителя» доставки используйте
  `nordic_uart_send_to()` / `nordic_uart_send_except()` в коде приложения.
- По умолчанию до **4** клиентов (лимит контроллера/стека ESP32-S3 выше;
  поднимите `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`, если нужно больше).
- Рация должна оставаться запитанной; при внезапном USB-разрыве прошивка
  автоматически переподключается к CDC-устройству.
