# v1.10.0 — Initial Release

ESP32-S3 firmware bridging a **MUST PV18-3024 VPM II** solar inverter
(Modbus RTU / RS485) to **Home Assistant** via MQTT, with a local web UI
for configuration, live logs, and history graphs.

## Highlights

- **Modbus RTU over RS485**, protocol confirmed against a real bus
  capture (slave `4`, function `03`/`06`, `19200` baud). Neighbouring
  registers are batched into a single request — ~80 registers read in
  just 6 Modbus transactions per poll cycle.
- **Home Assistant MQTT Discovery** — sensors, sliders, switches and
  dropdowns all appear automatically, no YAML configuration required.
- **Bidirectional control**: writable registers can be changed live
  from Home Assistant. Writes are confirmed synchronously against the
  inverter and the entity's state rolls back automatically in the UI
  if the inverter rejects or doesn't answer.
- **Enum & bitmask decoding** — operating-mode registers and
  error/warning bitmasks are decoded into readable text
  (`"PV voltage is too low"` instead of `32`), fully configurable via
  JSON without a firmware rebuild.
- **Non-blocking architecture** — Modbus polling runs on a dedicated
  FreeRTOS task (core 0); the web server, MQTT client, OTA endpoint and
  status LED (core 1) are never blocked by a slow or failing bus
  transaction.
- **Local history** in PSRAM (default 24h, up to 72h) with a table and
  graph served from the device's own web page — works independently of
  MQTT/Home Assistant, even from the WiFi setup access point with no
  internet access.
- **RGB status LED**, OTA updates (ElegantOTA), optional SHT41
  ambient temperature/humidity sensor, HTTP Basic Auth for the web
  UI/API, and optional TLS for MQTT.
- Runtime-editable register map — add, remove or retune any register
  (address, scale, unit, enum/bitmask decoding, writable range) from
  the web UI, no reflashing needed.

## Hardware

- ESP32-S3 (developed against an ESP32-S3-DevKitC-1 N16R8 clone —
  16MB flash / 8MB PSRAM; adjust `platformio.ini` for other variants).
- RS485-to-TTL adapter (auto-direction preferred).
- Optional: WS2812/NeoPixel status LED, SHT41 I²C sensor.

USB Host was evaluated and deliberately dropped — a stock
ESP32-S3-DevKitC-1 doesn't supply VBUS on its native OTG port, which is
a hardware limitation, not something fixable in firmware. RS485 has no
such constraint and needs no extra board beyond a cheap TTL adapter.

## Known limitations

- Register writes only support single 16-bit registers (Modbus
  function 06); 32-bit registers are read-only.
- `InverterErrorMessage` is a 4-word (64-bit) bitmask; only the first
  32 bits are read, and no bit-level decoding is provided by default
  for it (the upstream reference list has a transcription bug that
  left the exact word/bit layout for this specific field uncertain).
- A couple of registers from the reference map are non-functional
  stubs on this inverter revision (e.g. `BatteryStateOfHealth` always
  reads 100% — there's no BMS/CAN link on this setup to source it
  from). Worth empirically verifying any register you're not 100%
  sure about.

## Credits

The default register map is derived from the openly documented
[mukaschultze/ha-must-inverter](https://github.com/mukaschultze/ha-must-inverter)
(MIT), tested by its authors against the same inverter model.
