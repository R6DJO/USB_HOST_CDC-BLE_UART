# AGENT.md

Quick orientation for an AI agent working in this repo. Read this before editing.
This is a **binary-transparent multi-interface serial bridge** for ESP32-S3.

## TL;DR

```
                 app_main (main.c — pure orchestrator, ~50 lines)
                          │
        ┌─────────────────┴──────────────────┐
   msg_router (hub)            interface components — each self-registers its sink
        │        ┌──────────┬──────────┬──────────────┐
        │   usb_cdc    ble_bridge   uart_bridge
        │      │           │            ├─ UART Radio (AT, auto-baud)
        │      ▼           ▼            └─ UART Lora  (fixed baud)
        │  usb_host_   nordic_uart_
        │  cdc_acm     uart_multi   (NUS driver — kept router-free on purpose)
        │  (managed)
```

4 symmetric interfaces — USB CDC (Tyt MD-9600 radio `0x1FC9:0x0094`), BLE Nordic
UART Service (≤4 peers), UART Radio (AT, auto-baud 9600/115200), UART Lora. Every
interface is both a **source** (`msg_submit`) and a **sink** (router callback). By
default each frame is forwarded to all other interfaces; per-source rules live in
the `switch` in `route_message()` (`components/msg_router/src/msg_router.c`).

## Build & flash

ESP-IDF **v5.5.4** at `/home/r6djo/_espressif/v5.5.4/esp-idf`. Target **esp32s3**.

```bash
source /home/r6djo/_espressif/v5.5.4/esp-idf/export.sh   # puts idf.py on PATH
idf.py build                                              # or: set-target esp32s3 first
idf.py -p PORT flash monitor                              # exit monitor: Ctrl-]
```

**Env gotcha:** if `idf.py` complains the Python venv is missing, bootstrap it once:
`python /home/r6djo/_espressif/v5.5.4/esp-idf/tools/idf_tools.py install-python-env`,
then re-source `export.sh`. Xtensa GCC for offline syntax checks lives under
`~/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/.../bin/xtensa-esp32s3-elf-gcc`
(harvest `-I` flags from `build/compile_commands.json`).

`build/`, `managed_components/`, `dependencies.lock` are gitignored. `sdkconfig`
**is** committed; `sdkconfig.defaults` pins the intended defaults.

## Critical invariants (do NOT break these)

1. **Sink self-registration ordering.** Each interface component calls
   `msg_router_register_sink()` inside its `*_init()`. Therefore `*_init()` must run
   **after `msg_router_init()` and before `msg_router_start()`** (see `app_main`).
   Adding a sink = add a `*_init()` call in that window.
2. **Self-exclusion.** A sink must never echo a frame back to its originator.
   Single-port sinks (USB, UART Radio, UART Lora) early-return when
   `origin->iface == own_iface`. BLE is multi-peer, so it forwards to every peer
   **except** the sender (`origin->peer` = conn_handle → `nordic_uart_send_except`).
3. **Sinks may block** (USB tx up to 1 s, BLE notify with retries). That is exactly
   why each sink has its own task + private queue. **Never call a sink delivery fn
   from the routing task or a producer** — go through `msg_submit`/the per-sink queue.
4. **`cdc_dev` race discipline** (`components/usb_cdc`). The handle is touched from 3
   contexts; `s_cdc_mutex` (a FreeRTOS **mutex**, for priority inheritance) serializes
   TX vs close. Hold it across blocking TX; on disconnect null-under-lock first, then
   close **outside** the lock. Publish only a fully-configured handle (open+configure
   on a local, then publish under lock). Do not regress this.
5. **Soft-fail in runtime paths.** Do **not** add `ESP_ERROR_CHECK` in recoverable
   paths (open/setup/close/reconnect) — it aborts→reboots and defeats reconnect logic.
   Log + retry instead. `ESP_ERROR_CHECK` is reserved for genuinely fatal boot-time
   init (e.g. `usb_host_install`).
6. **Binary-transparent.** No CR/LF translation, no line buffering, no protocol
   parsing on the data path. The router copies raw bytes.
7. **`nordic_uart_multi` stays router-free.** It is a reusable NUS driver. BLE↔router
   glue lives in `ble_bridge`, not in the driver. Don't make the driver depend on
   `msg_router`.

## Configuration philosophy

Everything tunable is a **Kconfig** option under *Component config → …*, pinned in
`sdkconfig.defaults`. When you add a param: add it to the component's `Kconfig`
(with a sane default), pin it in `sdkconfig.defaults`, and document it in both
`README.md` and `README.ru.md`. Defaults in code `#define`s are only for things not
worth surfacing (task names, magic timeouts).

## Concurrency & memory

- **~13 tasks.** Priorities: `usb_lib` 20 → `msg_routing` 8 → UART RX 6 →
  sinks + `ble2rt` 5 → autobaud 4. Routing (8) > sinks (5) so dispatch never starves.
- **Locks:** `portMUX` (spinlock, tiny sections, no blocking inside) for BLE peer
  state and router drop counters. `s_cdc_mutex` (mutex) for the USB handle.
  `xRingbufferSendAcquire`/`Complete` for zero-copy frame handoff.
- **Drop semantics:** producers wait up to 20 ms on the central buffer then drop;
  per-sink queues drop-newest immediately (timeout 0) so a slow sink can't stall
  others. All drops are counted (per-source + per-destination) and logged every
  `CONFIG_MSG_ROUTER_STATS_PERIOD_MS` when changed; on demand via
  `msg_router_log_stats()`.
- **BLE delivery is atomic per frame** (`nus_peer_send` pre-allocates all chunk
  mbufs before notifying; on ENOMEM it sends nothing). `ble_gatts_notify_custom`
  takes ownership of the mbuf on **every** return path — never free one you passed.

## Where things live

| Want to… | Edit |
|---|---|
| Change routing rules (who gets what) | `route_message()` in `components/msg_router/src/msg_router.c` |
| Add a new interface | new component under `components/`, `*_init()` that self-registers a sink + spawns RX, call it in `app_main` before `msg_router_start()` |
| Change USB VID/PID/baud | `CONFIG_USB_CDC_*` (Kconfig) |
| Change BLE device name | `CONFIG_BLE_DEVICE_NAME` (Kconfig) |
| Change UART pins/baud/probe | `CONFIG_UART_RADIO_*` / `CONFIG_UART_LORA_*` (Kconfig) |
| Tune buffer/queue/stack sizes | `CONFIG_MSG_ROUTER_*` (Kconfig) |
| Advertised BLE service/UUIDs | `nordic_uart_multi.c` (NUS UUIDs) |

## Commit style

Conventional Commits: `feat:`, `feat(scope):`, `fix(scope):`, `refactor:`,
`docs:`. Keep README **both** `README.md` (en) and `README.ru.md` (ru) in sync —
Russian is the primary audience, English mirrors it. Always verify
`idf.py build` is green before committing.

## Already addressed (don't re-suggest)

cdc_dev tx/close race · USB ESP_ERROR_CHECK fatalism (now soft-fail) · UART-radio
hot-plug re-detection claim (now bounded attempts, documented) · silent drop loss
(now counted + logged) · BLE partial-frame delivery (now atomic) · ble2rt priority ·
len>0xFFFF silent truncation (now rejected) · USB open-log spam · RX stack sizing ·
main.c business logic (now pure orchestrator). The only known open item is the
theoretical post-open setup window in `usb_cdc` (line-coding on `new_dev` not under
the mutex) — left intentionally; the window is tiny and the CDC driver is presumed
internally synchronized.
