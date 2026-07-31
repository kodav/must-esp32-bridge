# Project status — MUST PV18-3024 VPM II bridge

Snapshot as of firmware **v1.13.0**. Use this instead of scrolling the
whole conversation history.

## Hardware / wiring (as built)

- ESP32-S3-DevKitC-1 N16R8 clone (16MB flash QIO, 8MB PSRAM OPI).
- Inverter connected via **RS485 only** (USB Host was tried and abandoned
  — stock board doesn't supply VBUS on the native OTG port, a hardware
  limitation, not fixable in firmware).
- RS485-TTL module (auto-direction), wired to the inverter's "COM" RJ45
  port: pin 1=B, pin 2=A, pin 3=GND. ESP32 side: GPIO16=RX, GPIO17=TX,
  `uart_de_pin: -1` (no DE/RE needed).
- SHT41 temperature/humidity sensor on I²C, GPIO8=SDA, GPIO9=SCL.
- WS2812 RGB status LED on GPIO48 (confirmed by Espressif's own board
  docs for this exact board).
- Modbus RTU confirmed by a real bus capture: slave ID `4`, baud
  `19200`, function `03`/`06`. **Current poll interval: 10000ms (10s)**
  — changed from the 15000ms default.

## Firmware architecture

- `framework = arduino` (plain, not the hybrid `arduino,espidf` mode —
  that was only needed for the abandoned USB Host attempt and added a
  lot of sdkconfig-wrangling pain; dropped entirely).
- **Three FreeRTOS contexts**:
  - Core 1 (Arduino main `loop()`): web server, OTA, status LED.
  - Core 0, Modbus task: register reads (batched — ~79 registers in ~6
    requests/cycle) and writes (`modbusWriteRegisterSync`, synchronous
    with a 3s timeout, used by both the removed manual write endpoint
    and the MQTT command handler).
  - Core 0, MQTT task: connect/reconnect/loop/discovery/publish, added
    late (v1.12.0) after a real-world incident where a long broker
    outage made the *whole web UI* unresponsive because MQTT's blocking
    `connect()` shared the main loop with the web server. Now fully
    isolated — the web server can't be blocked by MQTT no matter how
    long a broker outage lasts.
- `g_config` replacement (on config save) is protected by briefly
  suspending both the Modbus and MQTT tasks rather than a mutex — config
  changes are rare and always followed by a reboot.
- Local history: ring buffer **in PSRAM only, not persisted to flash**
  — deliberate tradeoff (flash wear + write latency at this write
  frequency). Every reboot (including OTA/reflash) resets it to empty.
  At the current 79 registers / 10s poll interval, real capacity is
  **~29-30h theoretical, ~24-25h recommended safe setting** before
  PSRAM allocation would fail (the code's hard cap is 72h but that's
  not actually reachable at this register count/interval — it would
  just fail to allocate and log a warning, not crash).
- `GET /api/history/export.csv` streams the *entire* buffer (all
  registers, no downsampling) directly from PSRAM in ~4KB chunks —
  confirmed working; a "only 28 seconds of data" report turned out to
  just be a freshly-reset buffer right after a config save, not a bug.
- Web UI is bilingual (EN default, RU toggle, `localStorage`-remembered),
  client-side `[data-i18n]` dictionary, no separate build per language.
- HTTP Basic Auth available for the whole web UI/API (disabled by
  default — empty password = no protection).
- `README.md` is English-only going forward (per explicit instruction);
  no more Russian README maintained.

## Current live register list (~79 entries)

- 14 writable **number** sliders (voltage/current setpoints — Absorb/
  Float/Absorption voltage, BatteryAh, current limits, etc.)
- 5 **switch** entities: `ChargerWorkEnable`, `BatteryEqualizationEnable`,
  `InverterOffgridWorkEnable`, `InverterSearchModeEnable`,
  `SolarPowerBalance` (4 more exist upstream with `enabled:False` in the
  reference map — not added, available on request).
- 3 **select** entities: `BatteryType` (currently reads "User defined" —
  expected, matches the manually-tuned voltage setpoints rather than a
  stock profile), `ChargerSourcePriority`, `EnergyUseMode` (the latter's
  exact code→text mapping is a best-guess from common Voltronic-family
  convention, not confirmed against this specific firmware — verify
  empirically if it matters).
- **Enum sensors**: `WorkState` (confirmed reading "Bypass", consistent
  with no solar yet / grid passthrough), `ChargerWorkstate`, `MpptState`,
  `ChargingState`.
- **Bitmask sensors**: `ChargerErrorMessage` (confirmed working — read
  "PV voltage is too low" while `PvVoltage=0`, correct), `ChargerWarningMessage`,
  `InverterWarningMessage`. `InverterErrorMessage` is deliberately left
  as a plain number (0=OK) — it's a 4-word/64-bit mask and the upstream
  reference list has a transcription bug that left the exact word/bit
  layout uncertain; only the first 32 bits are even read.
- `StateOfCharge` (address 113, borrowed from the PV1900-series map,
  trial) — **still unresolved**: read 100% consistently, plausible given
  battery state at the time but not proven live-tracking (no CAN/BMS
  link on this setup to source a real SOC from, so it's likely either a
  coulomb-counter estimate now that `BatteryAh` is corrected to 100, or
  a stub). Needs a longer observation window during real discharge to
  confirm one way or the other.
- `BatteryStateOfHealth` — **removed**, confirmed non-functional stub
  (always 100%, no BMS/CAN source available).
- `ExternalTemperature`, `DcRadiatorTemperature`, `AccumulatedTime` —
  **removed**, all read constant 0 on this unit.
- `AmbientTemperature` / `AmbientHumidity` — SHT41, `is_local: true`
  (not polled over Modbus).

## Things worth double-checking on the inverter's own LCD panel

- `InverterMaxDischargerCurrent` slider range capped conservatively at
  `1.0–15.0` (real reading 13.6A; nameplate AC output for this exact
  model is 13A) — the LCD menu for this specific setting should show
  the firmware's actual allowed range directly, more authoritative than
  any spec sheet.
- `BatteryLowVoltage` reads 17.0V — scale looks internally consistent
  with sibling registers (not a scale bug), but this is a *charger-side*
  threshold, distinct from `InverterBatteryLowVoltage` (22.0V, the one
  that actually disconnects load). Worth confirming neither is a
  leftover lead-acid-era setting.

## Deferred / not yet done

- Solar panels not installed yet. Once they are: watch `MpptState` /
  `ChargerCurrent` / `ChargerPower` come alive, re-verify
  `ChargerSourcePriority`/`EnergyUseMode` codes against real LCD
  behavior, and use the independent phase-matched Zigbee energy meter
  (already validated against `GridVoltage`/`GridCurrent`/`PLoad` in the
  no-solar case) to cross-check `Grid` vs `Load` diverging once real
  generation starts.
- `InverterErrorMessage` full 64-bit bit-level decoding — not done,
  needs the word/bit layout confirmed against a real fault first.
- Persisted (flash-backed) history surviving reboots — not built, would
  be a separate, more involved feature (periodic snapshot vs. flash
  wear/write-latency tradeoff) if ever needed.
- 4 more optional switch registers exist upstream (disabled by default
  in the reference map) — not added.

## Repo

`https://github.com/kodav/must-esp32-bridge` — README.md is the public,
English-only entry point; this file is internal working notes, not part
of the repo unless you want to add it.
