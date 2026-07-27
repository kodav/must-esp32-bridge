# MUST PV18-3024 VPM II → MQTT → Home Assistant

An ESP32-S3 firmware bridge between a **MUST PV18-3024 VPM II** off-grid/hybrid
solar inverter (protocol-compatible with the wider PH1800/PV1800/EP1800 family)
and **Home Assistant**: polls the inverter over Modbus RTU (RS485), publishes
readings to MQTT with automatic Home Assistant MQTT Discovery, exposes a local
web UI for configuration, live logs, local history graphs, and lets you write
settings back to the inverter — either through a REST endpoint or directly as
interactive Home Assistant `number` / `switch` / `select` entities.

## Features

- **Modbus RTU over RS485** — confirmed against a real bus capture (slave ID
  `4`, function `03`/`06`, baud `19200`).
- **Batched register reads** — neighbouring registers are read in a single
  Modbus request instead of one request per register (typically ~80 registers
  in 6 requests per poll cycle).
- **Non-blocking architecture** — Modbus polling runs in its own FreeRTOS task
  pinned to core 0; the web server, MQTT client, OTA and status LED run on
  core 1 and are never blocked by a slow/failing Modbus transaction.
- **Home Assistant MQTT Discovery** — sensors, writable numbers (sliders),
  switches and selects (dropdowns) are all auto-created, no YAML needed.
- **Bidirectional control** — writable registers can be changed live from
  Home Assistant (slider/toggle/dropdown), confirmed synchronously against the
  inverter, with automatic rollback in the UI if the write fails.
- **Enum & bitmask decoding** — registers that encode state machines
  (`WorkState`, `ChargerWorkstate`, ...) or error/warning bitmasks are decoded
  into human-readable text before publishing, entirely configurable via JSON
  (no firmware rebuild needed to add/adjust a mapping).
- **Local history** — a ring buffer in PSRAM keeps recent readings and serves
  a simple table/graph view from the device's own web page, independent of
  MQTT/Home Assistant, and works even from the WiFi setup AP with no internet
  access (plain canvas, no CDN dependencies).
- **RGB status LED** — at-a-glance device health (WiFi/MQTT/Modbus/OTA state)
  without needing to open the web UI.
- **Optional SHT41 ambient sensor** — temperature/humidity at the inverter's
  installation site, published through the same MQTT/history pipeline.
- **OTA updates** via [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA).
- **HTTP Basic Auth** for the web UI and API (disabled by default).
- **TLS support** for MQTT (`WiFiClientSecure`, useful when the broker is
  reachable only over the public internet).

## Hardware

- **ESP32-S3** (tested on an ESP32-S3-DevKitC-1 N16R8 clone: 16MB flash QIO +
  8MB PSRAM OPI — adjust `platformio.ini` for other flash/PSRAM variants).
- **RS485-to-TTL adapter** (auto-direction preferred, e.g. based on a
  MAX13487-style chip — avoids needing a DE/RE control pin).
- Optional: WS2812/NeoPixel RGB LED (GPIO48 on stock ESP32-S3-DevKitC-1,
  confirmed by Espressif's official board documentation), SHT41 I²C sensor.

### Why not USB Host?

An earlier iteration of this project attempted to talk to the inverter's
USB-B port directly, with the ESP32-S3 acting as a USB host. This was
abandoned: a **stock ESP32-S3-DevKitC-1 does not supply VBUS (+5V) on its
native OTG port** — it's wired for the chip to act as a USB *device*, not a
host, and the inverter's internal USB-serial chip never powers/announces
itself without VBUS present. This is a hardware limitation of the board, not
something fixable in firmware (would need external 5V wired to the cable's
VBUS pin, or a purpose-built board like the ESP32-S3-USB-OTG with proper power
switching). RS485 has no such limitation and needs no extra board beyond a
cheap RS485-to-TTL module.

## Wiring (RS485)

On this inverter, RS485 is exposed on the **"COM" port (RJ45)**:

| RJ45 pin | Signal |
|---|---|
| 1 | RS485 B |
| 2 | RS485 A |
| 3 | GND |
| 4–8 | unused |

```
RJ45 pin 2 (A)  -> RS485 module A+
RJ45 pin 1 (B)  -> RS485 module B-
RJ45 pin 3 (GND)-> RS485 module GND
module RXD      -> ESP32 GPIO17 (TX)
module TXD      -> ESP32 GPIO16 (RX)
module VCC      -> ESP32 3.3V or 5V (check your module)
module GND      -> ESP32 GND
```

## Building & flashing

```
pio run -t upload
pio device monitor
```

On first boot (no WiFi configured yet) the device starts an access point
`MUST-PV18-Setup` (password `12345678`). Connect to it and open
`http://192.168.4.1/` to configure WiFi, MQTT and Modbus settings. The device
reboots and joins your network once saved.

Subsequent updates can be done over OTA at `http://<device-ip>/update`
(ElegantOTA web UI) instead of a USB cable.

## Register map

The default 60+ register set is derived from the openly documented map in
[mukaschultze/ha-must-inverter](https://github.com/mukaschultze/ha-must-inverter)
(MIT), which was tested specifically against a PV18-3024 VPM unit. Everything
is editable at runtime from the web UI — no rebuild needed to add, remove or
retune a register:

```json
{
  "name": "GridVoltage",
  "name_ru": "Напряжение сети",
  "address": 25207,
  "count": 1,
  "is_signed": false,
  "scale": 0.1,
  "unit": "V",
  "writable": false
}
```

| Field | Meaning |
|---|---|
| `name` | Technical name — used in the MQTT topic and `unique_id`. Don't rename after first Home Assistant discovery, or you'll get a duplicate entity. |
| `name_ru` | Display name shown in Home Assistant. Freely editable at any time. |
| `address`, `count` | Modbus holding-register address and width (1 or 2 words). |
| `is_signed` | Interpret raw value as signed (`int16`/`int32`). |
| `scale` | Multiplier applied to the raw register value. |
| `unit` | Unit of measurement string. |
| `writable` | Exposes the register as an editable Home Assistant entity (see below). |
| `min` / `max` / `step` | Range for a `number` slider (required when `writable` and no `enum_map`/`is_switch`). |
| `is_switch` | Publishes as a Home Assistant `switch` instead of a `number`. |
| `enum_map` | `{"code": "text"}` — decodes a fixed-value register into text. Combined with `writable: true` it becomes a Home Assistant `select` dropdown. |
| `bit_map` | `{"bit": "text"}` — decodes a bitmask register (e.g. error/warning flags) into a comma-separated list of active flags, or `"OK"` if none are set. |
| `is_local` | Value comes from a non-Modbus source (e.g. the onboard SHT41 sensor) instead of being polled over the bus. |

## Home Assistant entity types

| Register flags | HA platform |
|---|---|
| `writable: false` (default), no `enum_map`/`bit_map` | `sensor` |
| `writable: false`, `enum_map` set | `sensor` with `device_class: enum` |
| `writable: false`, `bit_map` set | plain text `sensor` (comma-separated active flags, or `"OK"`) |
| `writable: true`, no `is_switch`/`enum_map` | `number` (slider) |
| `writable: true`, `is_switch: true` | `switch` |
| `writable: true`, `enum_map` set | `select` (dropdown) |

Writes are performed synchronously against the inverter (default 3s timeout)
and the confirmed state is republished immediately — no waiting for the next
poll cycle to see whether a change actually took effect.

## Web UI language

The configuration page is bilingual (English/Russian) with a toggle
button (`EN`/`RU`) in the header — translations are applied client-side
from a small dictionary, no separate build per language. The choice is
remembered in the browser (`localStorage`) and defaults to Russian if
the browser's language is Russian, English otherwise.

## Architecture notes

- **Dedicated Modbus task (core 0)**: reading/writing registers happens
  entirely off the main loop. Results and write requests cross task
  boundaries via small FreeRTOS queues carrying only a register index + value
  (no strings/pointers), so no mutex is needed around MQTT publishing or the
  local history buffer — they're only ever touched from the main-loop
  context. `g_config` itself is protected by briefly suspending the Modbus
  task around a full config replacement (config changes are rare and always
  followed by a reboot anyway), rather than a pervasive mutex.
- **`log_buffer.cpp`** intercepts all `ESP_LOG` output system-wide (WiFi,
  TLS, etc.) in addition to the firmware's own log calls, and is guarded by a
  recursive FreeRTOS mutex since multiple tasks can log concurrently.
- **PSRAM is required** for the local history buffer (defaults to 24h,
  capped at 72h) — sized automatically from the current register count and
  poll interval.

## Known limitations

- Writes only support single 16-bit registers (Modbus function 06). 32-bit
  (`count: 2`) registers are read-only in this project.
- `InverterErrorMessage` is a 4-word (64-bit) bitmask; only the first 32 bits
  (2 words) are read, and no `bit_map` is provided by default for it — the
  upstream register list has a transcription bug that made the exact
  word/bit layout for this specific field uncertain, so it's left as a plain
  "0 = OK, anything else = investigate" number rather than guessing.
- A couple of registers imported from the reference map turned out to be
  non-functional stubs on this particular inverter revision (always read a
  constant value, e.g. `BatteryStateOfHealth` always reading 100% with no
  BMS/CAN link present to source it from) — worth empirically double-checking
  any register you're not 100% sure about before relying on it.

## License

The register map is derived from `mukaschultze/ha-must-inverter` (MIT). Add
your preferred license for the rest of this project here.
