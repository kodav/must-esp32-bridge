#pragma once

// Embedded configuration/logs web page.
// Served directly from flash (PROGMEM), no separate filesystem needed
// for static assets -- the whole firmware is a single .bin for OTA.
// Bilingual UI (EN/RU), toggle in the header, translations applied
// client-side via [data-i18n] attributes -- see the I18N dictionary
// near the bottom of the <script> block.

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MUST PV18 Bridge</title>
<style>
  :root { --bg:#0f1115; --card:#171a21; --border:#2a2f3a; --text:#e6e8eb;
          --muted:#8b93a3; --accent:#4f8cff; --ok:#3ecf6c; --err:#ff5d5d; }
  * { box-sizing: border-box; }
  body { margin:0; font-family: -apple-system, Segoe UI, Roboto, sans-serif;
         background: var(--bg); color: var(--text); }
  header { padding: 16px 20px; border-bottom: 1px solid var(--border);
           display:flex; align-items:center; justify-content:space-between; gap:12px; }
  header h1 { font-size: 16px; margin:0; font-weight:600; }
  header .right { display:flex; align-items:center; gap:14px; }
  #lang_toggle { background:#2a2f3a; color:var(--text); border:none; border-radius:6px;
                 padding:6px 10px; font-size:12px; cursor:pointer; margin-top:0; }
  .wrap { max-width: 860px; margin: 0 auto; padding: 20px; }
  .card { background: var(--card); border:1px solid var(--border);
          border-radius: 10px; padding: 16px 18px; margin-bottom: 16px; }
  .card h2 { font-size: 14px; text-transform: uppercase; letter-spacing:.04em;
             color: var(--muted); margin: 0 0 12px; }
  label { display:block; font-size: 13px; color: var(--muted); margin: 10px 0 4px; }
  input[type=text], input[type=password], input[type=number], textarea, select {
    width:100%; padding:8px 10px; background:#0f1115; border:1px solid var(--border);
    border-radius:6px; color:var(--text); font-size:14px; font-family: inherit; }
  textarea { min-height: 220px; font-family: ui-monospace, monospace; font-size: 12px; }
  .row { display:flex; gap:12px; flex-wrap:wrap; }
  .row > div { flex: 1; min-width: 160px; }
  button { background: var(--accent); color:white; border:none; border-radius:6px;
           padding: 10px 16px; font-size: 14px; cursor:pointer; margin-top: 14px; }
  button.secondary { background: #2a2f3a; }
  button:hover { filter: brightness(1.1); }
  #status { font-size: 13px; margin-top: 10px; min-height: 18px; }
  #status.ok { color: var(--ok); }
  #status.err { color: var(--err); }
  #log { background:#0a0c10; border:1px solid var(--border); border-radius:8px;
         padding:10px; height: 260px; overflow-y:auto; font-family: ui-monospace, monospace;
         font-size: 12px; white-space: pre-wrap; }
  .hint { color: var(--muted); font-size: 12px; margin-top: 4px; }
  a.link { color: var(--accent); text-decoration: none; }
</style>
</head>
<body>
<header>
  <h1>MUST PV18-3024 VPM II &rarr; MQTT <span id="fw_version" style="color:var(--muted);font-weight:400;font-size:12px;"></span></h1>
  <div class="right">
    <a class="link" href="/update" data-i18n="link_ota">Firmware (OTA) &rarr;</a>
    <button id="lang_toggle" onclick="toggleLang()">RU</button>
  </div>
</header>
<div class="wrap">

  <div class="card">
    <h2 data-i18n="card_wifi">WiFi</h2>
    <div class="row">
      <div><label data-i18n="label_ssid">SSID</label><input id="wifi_ssid" type="text"></div>
      <div><label data-i18n="label_password">Password</label><input id="wifi_password" type="password"></div>
    </div>
  </div>

  <div class="card">
    <h2>MQTT</h2>
    <div class="row">
      <div><label data-i18n="label_host">Host</label><input id="mqtt_host" type="text" placeholder="192.168.1.10"></div>
      <div><label data-i18n="label_port">Port</label><input id="mqtt_port" type="number" value="1883"></div>
    </div>
    <div class="row">
      <div><label data-i18n="label_user">Username</label><input id="mqtt_user" type="text"></div>
      <div><label data-i18n="label_password">Password</label><input id="mqtt_password" type="password"></div>
    </div>
    <div class="row">
      <div><label>Client ID</label><input id="mqtt_client_id" type="text"></div>
      <div><label data-i18n="label_topic_prefix">Topic prefix</label><input id="mqtt_topic_prefix" type="text"></div>
    </div>
    <label><input id="mqtt_ha_discovery" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">Home Assistant MQTT Discovery</label>
    <br>
    <label><input id="mqtt_tls" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;"><span data-i18n="label_tls">TLS (encryption, usually port 8883)</span></label>
    <p class="hint" data-i18n="hint_tls">Enable this if the MQTT port is exposed to the internet
      (e.g. a device at a remote site, broker reachable via a public IP) --
      otherwise the MQTT username/password travel in plain text the whole way.
      Without TLS, only use this on a local/trusted network.</p>
  </div>

  <div class="card">
    <h2 data-i18n="card_modbus">Modbus / RS485</h2>
    <div class="row">
      <div><label>Slave ID</label><input id="mb_slave" type="number" value="4"></div>
      <div><label>Baud rate</label><input id="mb_baud" type="number" value="19200"></div>
    </div>
    <div class="row">
      <div><label data-i18n="label_poll_interval">Poll interval, ms</label><input id="mb_interval" type="number" value="5000"></div>
    </div>
    <div class="row">
      <div><label data-i18n="label_uart_rx">UART RX pin</label><input id="mb_rx" type="number" value="16"></div>
      <div><label data-i18n="label_uart_tx">UART TX pin</label><input id="mb_tx" type="number" value="17"></div>
      <div><label data-i18n="label_de_pin">DE pin (-1 if not needed)</label><input id="mb_de" type="number" value="-1"></div>
    </div>
    <p class="hint" data-i18n="hint_de_pin">DE pin is only needed for RS485 modules without auto-direction
      (no auto chip like the MAX13487 family). For most off-the-shelf
      "RS485-TTL auto" modules, leave this at -1.</p>

    <label data-i18n="label_registers">Registers to poll (JSON array)</label>
    <textarea id="mb_registers" spellcheck="false"></textarea>
    <p class="hint" data-i18n="hint_registers">Fields: name (technical, Latin letters -- used in the MQTT topic
      and unique_id, don't rename after the first Home Assistant discovery,
      or you'll get a duplicate entity), name_ru (display name in Home
      Assistant, freely editable at any time), address, count (1 or 2),
      is_signed, scale, unit, writable (true/false -- whether to offer this
      register as a writable HA entity; false by default, i.e. read-only),
      enum_map (optional -- a code-&gt;text object, e.g.
      {"0":"On","4":"Bypass"} -- if set, MQTT/HA get text instead of a
      number, device_class becomes enum), bit_map (optional -- like
      enum_map, but the key is a BIT number, not a value -- for
      error/warning bitmasks; several bits can be active at once, MQTT
      gets a comma-separated list or "OK"), min/max/step (required when
      writable:true and no enum_map -- range for the Home Assistant
      slider, makes it a number entity instead of a plain sensor),
      is_switch (true -- the register becomes an HA switch instead of a
      number, for boolean 0/1 parameters), enum_map + writable:true
      together -- the register becomes an HA select (dropdown) instead
      of a sensor. Confirmed against a real inverter bus capture:
      slave=4, function=3 (Read Holding Registers), the working range
      starts at address 25200.</p>
  </div>

  <div class="card">
    <h2>OTA</h2>
    <label data-i18n="label_ota_password">Password for /update (empty = no password)</label>
    <input id="ota_password" type="password">
  </div>

  <div class="card">
    <h2 data-i18n="card_access">Web access</h2>
    <div class="row">
      <div><label data-i18n="label_login">Username</label><input id="web_username" type="text"></div>
      <div><label data-i18n="label_access_password">Password (empty = no protection)</label><input id="web_password" type="password"></div>
    </div>
    <p class="hint" data-i18n="hint_access">HTTP Basic Auth for the whole web UI and API (except
      /update, which has its own separate password above). Empty by
      default -- careful: once you set a password here and save, the
      browser will prompt for login/password on the next page load --
      don't forget them, or you'll have to reset the config from scratch.</p>
  </div>

  <div class="card">
    <h2 data-i18n="card_rgb">RGB status LED</h2>
    <label><input id="rgb_led_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;"><span data-i18n="label_enabled">Enabled</span></label>
    <div class="row">
      <div><label data-i18n="label_gpio_pin">GPIO pin</label><input id="rgb_led_pin" type="number" value="48"></div>
      <div><label data-i18n="label_brightness">Brightness (0-255)</label><input id="rgb_led_brightness" type="number" value="40"></div>
    </div>
    <p class="hint" data-i18n="hint_rgb">Green = everything OK. Red = inverter not responding.
      Orange = no MQTT connection. Blue = connecting to WiFi.
      Yellow = setup access point active. Purple = OTA update in progress.</p>
  </div>

  <div class="card">
    <h2 data-i18n="card_sht41">Temperature/humidity sensor (SHT41, I2C)</h2>
    <label><input id="sht41_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;"><span data-i18n="label_enabled">Enabled</span></label>
    <div class="row">
      <div><label data-i18n="label_sda">SDA pin</label><input id="sht41_sda_pin" type="number" value="8"></div>
      <div><label data-i18n="label_scl">SCL pin</label><input id="sht41_scl_pin" type="number" value="9"></div>
    </div>
    <p class="hint" data-i18n="hint_sht41">Measures ambient conditions where the ESP32/inverter is
      installed, unrelated to Modbus. Published as AmbientTemperature/
      AmbientHumidity through the same MQTT/history pipeline as regular
      registers.</p>
  </div>

  <div class="card">
    <h2 data-i18n="card_history_settings">Local history</h2>
    <label><input id="history_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;"><span data-i18n="label_history_enabled">Keep history (for the table/graph below)</span></label>
    <label data-i18n="label_history_hours">History depth, hours</label>
    <input id="history_hours" type="number" value="24" step="0.5">
    <p class="hint" data-i18n="hint_history">Stored in PSRAM independently of MQTT/WiFi -- you can view
      it even if MQTT/Home Assistant isn't configured at all. Actual
      capacity in entries depends on the number of registers and the
      poll interval, the exact figure is logged after saving. Time comes
      from NTP -- if WiFi hasn't synced the clock yet, the earliest
      entries may show an implausible date (around 1970).</p>
  </div>

  <div class="card">
    <h2 data-i18n="card_flash_history">Persistent history (flash, survives reboot)</h2>
    <label><input id="flash_history_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;"><span data-i18n="label_flash_history_enabled">Enabled</span></label>
    <label data-i18n="label_flash_history_interval">Checkpoint interval, minutes</label>
    <input id="flash_history_interval" type="number" value="5">
    <p class="hint" data-i18n="hint_flash_history">Unlike the RAM history above (every reading, lost on reboot),
      this writes one snapshot of all registers' latest values every N
      minutes to flash -- coarser, but survives reboots/reflashing. Off
      by default -- deliberate: flash has a limited write/erase
      lifespan, so this trades resolution for endurance rather than
      writing every single reading. At the default 5-minute interval,
      wear is negligible over the device's realistic lifetime.</p>
    <a class="link" href="/api/history/export_flash.csv" data-i18n="link_flash_csv_export">&#8595; Download persistent history as CSV</a>
  </div>

  <button onclick="saveConfig()" data-i18n="btn_save">Save and reboot</button>
  <button class="secondary" onclick="loadConfig()" data-i18n="btn_cancel">Discard changes</button>
  <div id="status"></div>

  <div class="card" style="margin-top:24px;">
    <h2 data-i18n="card_history_view">History: table and graph</h2>
    <div class="row">
      <div>
        <label data-i18n="label_register">Register</label>
        <select id="history_register"></select>
      </div>
      <div>
        <label data-i18n="label_points">Points (max 2000)</label>
        <input id="history_points" type="number" value="300">
      </div>
    </div>
    <button onclick="loadHistory()" data-i18n="btn_load">Load</button>
    <a class="link" href="/api/history/export.csv" style="margin-left:14px;font-size:14px;" data-i18n="link_csv_export">&#8595; Download all history as CSV</a>
    <canvas id="history_canvas" width="800" height="220" style="width:100%;height:220px;background:#0a0c10;border:1px solid var(--border);border-radius:8px;margin-top:12px;"></canvas>
    <div id="history_table_wrap" style="max-height:260px;overflow-y:auto;margin-top:12px;">
      <table style="width:100%;border-collapse:collapse;font-size:12px;">
        <thead><tr style="text-align:left;color:var(--muted);">
          <th style="padding:4px 8px;" data-i18n="th_time">Time</th><th style="padding:4px 8px;" data-i18n="th_value">Value</th>
        </tr></thead>
        <tbody id="history_table_body"></tbody>
      </table>
    </div>
  </div>

  <div class="card" style="margin-top:24px;">
    <h2 data-i18n="card_logs">Logs (live)</h2>
    <input id="log_filter" type="text" data-i18n-placeholder="placeholder_log_filter" placeholder="Filter (e.g.: usb, modbus, error)" style="margin-bottom:8px;">
    <div id="log"></div>
  </div>

</div>

<script>
const I18N = {
  en: {
    link_ota: "Firmware (OTA) \u2192",
    card_wifi: "WiFi",
    label_ssid: "SSID",
    label_password: "Password",
    label_host: "Host",
    label_port: "Port",
    label_user: "Username",
    label_topic_prefix: "Topic prefix",
    label_tls: "TLS (encryption, usually port 8883)",
    hint_tls: "Enable this if the MQTT port is exposed to the internet " +
      "(e.g. a device at a remote site, broker reachable via a public IP) -- " +
      "otherwise the MQTT username/password travel in plain text the whole way. " +
      "Without TLS, only use this on a local/trusted network.",
    card_modbus: "Modbus / RS485",
    label_poll_interval: "Poll interval, ms",
    label_uart_rx: "UART RX pin",
    label_uart_tx: "UART TX pin",
    label_de_pin: "DE pin (-1 if not needed)",
    hint_de_pin: "DE pin is only needed for RS485 modules without auto-direction " +
      "(no auto chip like the MAX13487 family). For most off-the-shelf " +
      "\"RS485-TTL auto\" modules, leave this at -1.",
    label_registers: "Registers to poll (JSON array)",
    hint_registers: "Fields: name (technical, Latin letters -- used in the MQTT topic " +
      "and unique_id, don't rename after the first Home Assistant discovery, " +
      "or you'll get a duplicate entity), name_ru (display name in Home " +
      "Assistant, freely editable at any time), address, count (1 or 2), " +
      "is_signed, scale, unit, writable (true/false -- whether to offer this " +
      "register as a writable HA entity; false by default, i.e. read-only), " +
      "enum_map (optional -- a code->text object, e.g. " +
      "{\"0\":\"On\",\"4\":\"Bypass\"} -- if set, MQTT/HA get text instead of a " +
      "number, device_class becomes enum), bit_map (optional -- like " +
      "enum_map, but the key is a BIT number, not a value -- for " +
      "error/warning bitmasks; several bits can be active at once, MQTT " +
      "gets a comma-separated list or \"OK\"), min/max/step (required when " +
      "writable:true and no enum_map -- range for the Home Assistant " +
      "slider, makes it a number entity instead of a plain sensor), " +
      "is_switch (true -- the register becomes an HA switch instead of a " +
      "number, for boolean 0/1 parameters), enum_map + writable:true " +
      "together -- the register becomes an HA select (dropdown) instead " +
      "of a sensor. Confirmed against a real inverter bus capture: " +
      "slave=4, function=3 (Read Holding Registers), the working range " +
      "starts at address 25200.",
    label_ota_password: "Password for /update (empty = no password)",
    card_access: "Web access",
    label_login: "Username",
    label_access_password: "Password (empty = no protection)",
    hint_access: "HTTP Basic Auth for the whole web UI and API (except " +
      "/update, which has its own separate password above). Empty by " +
      "default -- careful: once you set a password here and save, the " +
      "browser will prompt for login/password on the next page load -- " +
      "don't forget them, or you'll have to reset the config from scratch.",
    card_rgb: "RGB status LED",
    label_enabled: "Enabled",
    label_gpio_pin: "GPIO pin",
    label_brightness: "Brightness (0-255)",
    hint_rgb: "Green = everything OK. Red = inverter not responding. " +
      "Orange = no MQTT connection. Blue = connecting to WiFi. " +
      "Yellow = setup access point active. Purple = OTA update in progress.",
    card_sht41: "Temperature/humidity sensor (SHT41, I2C)",
    label_sda: "SDA pin",
    label_scl: "SCL pin",
    hint_sht41: "Measures ambient conditions where the ESP32/inverter is " +
      "installed, unrelated to Modbus. Published as AmbientTemperature/ " +
      "AmbientHumidity through the same MQTT/history pipeline as regular " +
      "registers.",
    card_history_settings: "Local history",
    label_history_enabled: "Keep history (for the table/graph below)",
    label_history_hours: "History depth, hours",
    hint_history: "Stored in PSRAM independently of MQTT/WiFi -- you can view " +
      "it even if MQTT/Home Assistant isn't configured at all. Actual " +
      "capacity in entries depends on the number of registers and the " +
      "poll interval, the exact figure is logged after saving. Time comes " +
      "from NTP -- if WiFi hasn't synced the clock yet, the earliest " +
      "entries may show an implausible date (around 1970).",
    card_flash_history: "Persistent history (flash, survives reboot)",
    label_flash_history_enabled: "Enabled",
    label_flash_history_interval: "Checkpoint interval, minutes",
    hint_flash_history: "Unlike the RAM history above (every reading, lost on reboot), " +
      "this writes one snapshot of all registers' latest values every N " +
      "minutes to flash -- coarser, but survives reboots/reflashing. Off " +
      "by default -- deliberate: flash has a limited write/erase " +
      "lifespan, so this trades resolution for endurance rather than " +
      "writing every single reading. At the default 5-minute interval, " +
      "wear is negligible over the device's realistic lifetime.",
    link_flash_csv_export: "\u2193 Download persistent history as CSV",
    btn_save: "Save and reboot",
    btn_cancel: "Discard changes",
    card_history_view: "History: table and graph",
    label_register: "Register",
    label_points: "Points (max 2000)",
    btn_load: "Load",
    link_csv_export: "\u2193 Download all history as CSV",
    th_time: "Time",
    th_value: "Value",
    card_logs: "Logs (live)",
    placeholder_log_filter: "Filter (e.g.: usb, modbus, error)",
    status_loaded: "Config loaded",
    status_json_error: "Error in registers JSON: ",
    status_saved: "Saved, rebooting...",
    status_save_error: "Save error: ",
    text_not_enough_data: "Not enough data for a graph",
    text_time_not_synced: "(time not synced)",
    text_ntp_warning: "time not synced (NTP)"
  },
  ru: {
    link_ota: "\u041f\u0440\u043e\u0448\u0438\u0432\u043a\u0430 (OTA) \u2192",
    card_wifi: "WiFi",
    label_ssid: "SSID",
    label_password: "\u041f\u0430\u0440\u043e\u043b\u044c",
    label_host: "\u0425\u043e\u0441\u0442",
    label_port: "\u041f\u043e\u0440\u0442",
    label_user: "\u041f\u043e\u043b\u044c\u0437\u043e\u0432\u0430\u0442\u0435\u043b\u044c",
    label_topic_prefix: "Topic prefix",
    label_tls: "TLS (\u0448\u0438\u0444\u0440\u043e\u0432\u0430\u043d\u0438\u0435, \u043f\u043e\u0440\u0442 \u043e\u0431\u044b\u0447\u043d\u043e 8883)",
    hint_tls: "\u0412\u043a\u043b\u044e\u0447\u0438\u0442\u0435, \u0435\u0441\u043b\u0438 MQTT-\u043f\u043e\u0440\u0442 \u043e\u0442\u043a\u0440\u044b\u0442 \u043d\u0430\u0440\u0443\u0436\u0443 \u0432 \u0438\u043d\u0442\u0435\u0440\u043d\u0435\u0442 -- " +
      "\u0438\u043d\u0430\u0447\u0435 \u043b\u043e\u0433\u0438\u043d/\u043f\u0430\u0440\u043e\u043b\u044c \u043e\u0442 MQTT \u0438\u0434\u0443\u0442 \u043e\u0442\u043a\u0440\u044b\u0442\u044b\u043c \u0442\u0435\u043a\u0441\u0442\u043e\u043c \u0447\u0435\u0440\u0435\u0437 \u0432\u0435\u0441\u044c \u043f\u0443\u0442\u044c. " +
      "\u0411\u0435\u0437 TLS \u0438\u0441\u043f\u043e\u043b\u044c\u0437\u0443\u0439\u0442\u0435 \u0442\u043e\u043b\u044c\u043a\u043e \u0432 \u043b\u043e\u043a\u0430\u043b\u044c\u043d\u043e\u0439 \u0441\u0435\u0442\u0438.",
    card_modbus: "Modbus / RS485",
    label_poll_interval: "\u0418\u043d\u0442\u0435\u0440\u0432\u0430\u043b \u043e\u043f\u0440\u043e\u0441\u0430, \u043c\u0441",
    label_uart_rx: "UART RX pin",
    label_uart_tx: "UART TX pin",
    label_de_pin: "DE pin (-1 \u0435\u0441\u043b\u0438 \u043d\u0435 \u043d\u0443\u0436\u0435\u043d)",
    hint_de_pin: "DE pin \u043d\u0443\u0436\u0435\u043d \u0442\u043e\u043b\u044c\u043a\u043e \u0434\u043b\u044f RS485-\u043c\u043e\u0434\u0443\u043b\u0435\u0439 \u0431\u0435\u0437 \u0430\u0432\u0442\u043e\u043d\u0430\u043f\u0440\u0430\u0432\u043b\u0435\u043d\u0438\u044f. \u0414\u043b\u044f \u0431\u043e\u043b\u044c\u0448\u0438\u043d\u0441\u0442\u0432\u0430 \u0433\u043e\u0442\u043e\u0432\u044b\u0445 \u043c\u043e\u0434\u0443\u043b\u0435\u0439 \u043e\u0441\u0442\u0430\u0432\u044c\u0442\u0435 -1.",
    label_registers: "\u0420\u0435\u0433\u0438\u0441\u0442\u0440\u044b \u0434\u043b\u044f \u043e\u043f\u0440\u043e\u0441\u0430 (JSON-\u043c\u0430\u0441\u0441\u0438\u0432)",
    hint_registers: "\u041f\u043e\u043b\u044f: name (\u0442\u0435\u0445\u043d\u0438\u0447\u0435\u0441\u043a\u043e\u0435 \u0438\u043c\u044f \u0432 MQTT-\u0442\u043e\u043f\u0438\u043a\u0435/unique_id, \u043d\u0435 \u043c\u0435\u043d\u044f\u0439\u0442\u0435 " +
      "\u043f\u043e\u0441\u043b\u0435 \u043f\u0435\u0440\u0432\u043e\u0439 \u043d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u0432 HA), name_ru (\u043e\u0442\u043e\u0431\u0440\u0430\u0436\u0430\u0435\u043c\u043e\u0435 \u0438\u043c\u044f), " +
      "address, count (1 \u0438\u043b\u0438 2), is_signed, scale, unit, writable, " +
      "enum_map, bit_map, min/max/step, is_switch -- \u043f\u043e\u0434\u0440\u043e\u0431\u043d\u043e\u0441\u0442\u0438 \u0441\u043c. \u0432 README. " +
      "\u041f\u043e\u0434\u0442\u0432\u0435\u0440\u0436\u0434\u0435\u043d\u043e \u0434\u0430\u043c\u043f\u043e\u043c \u0440\u0435\u0430\u043b\u044c\u043d\u043e\u0433\u043e \u0438\u043d\u0432\u0435\u0440\u0442\u043e\u0440\u0430: slave=4, function=3.",
    label_ota_password: "\u041f\u0430\u0440\u043e\u043b\u044c \u0434\u043b\u044f /update (\u043f\u0443\u0441\u0442\u043e = \u0431\u0435\u0437 \u043f\u0430\u0440\u043e\u043b\u044f)",
    card_access: "\u0414\u043e\u0441\u0442\u0443\u043f \u043a \u0432\u0435\u0431-\u0438\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441\u0443",
    label_login: "\u041b\u043e\u0433\u0438\u043d",
    label_access_password: "\u041f\u0430\u0440\u043e\u043b\u044c (\u043f\u0443\u0441\u0442\u043e = \u0431\u0435\u0437 \u0437\u0430\u0449\u0438\u0442\u044b)",
    hint_access: "HTTP Basic Auth \u043d\u0430 \u0432\u0435\u0441\u044c \u0432\u0435\u0431-\u0438\u043d\u0442\u0435\u0440\u0444\u0435\u0439\u0441 \u0438 API (\u043a\u0440\u043e\u043c\u0435 /update). " +
      "\u041f\u0443\u0441\u0442\u043e \u043f\u043e \u0443\u043c\u043e\u043b\u0447\u0430\u043d\u0438\u044e -- \u043d\u0435 \u0437\u0430\u0431\u0443\u0434\u044c\u0442\u0435 \u043f\u0430\u0440\u043e\u043b\u044c \u043f\u043e\u0441\u043b\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438.",
    card_rgb: "RGB-\u0438\u043d\u0434\u0438\u043a\u0430\u0442\u043e\u0440",
    label_enabled: "\u0412\u043a\u043b\u044e\u0447\u0451\u043d",
    label_gpio_pin: "GPIO \u043f\u0438\u043d",
    label_brightness: "\u042f\u0440\u043a\u043e\u0441\u0442\u044c (0-255)",
    hint_rgb: "\u0417\u0435\u043b\u0451\u043d\u044b\u0439 = \u0432\u0441\u0451 \u041e\u041a. \u041a\u0440\u0430\u0441\u043d\u044b\u0439 = \u0438\u043d\u0432\u0435\u0440\u0442\u043e\u0440 \u043d\u0435 \u043e\u0442\u0432\u0435\u0447\u0430\u0435\u0442. " +
      "\u041e\u0440\u0430\u043d\u0436\u0435\u0432\u044b\u0439 = \u043d\u0435\u0442 \u0441\u0432\u044f\u0437\u0438 \u0441 MQTT. \u0421\u0438\u043d\u0438\u0439 = \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0438\u0435 \u043a WiFi. " +
      "\u0416\u0451\u043b\u0442\u044b\u0439 = \u0442\u043e\u0447\u043a\u0430 \u0434\u043e\u0441\u0442\u0443\u043f\u0430. \u0424\u0438\u043e\u043b\u0435\u0442\u043e\u0432\u044b\u0439 = \u0438\u0434\u0451\u0442 OTA.",
    card_sht41: "\u0414\u0430\u0442\u0447\u0438\u043a \u0442\u0435\u043c\u043f\u0435\u0440\u0430\u0442\u0443\u0440\u044b/\u0432\u043b\u0430\u0436\u043d\u043e\u0441\u0442\u0438 (SHT41, I2C)",
    label_sda: "SDA \u043f\u0438\u043d",
    label_scl: "SCL \u043f\u0438\u043d",
    hint_sht41: "\u0418\u0437\u043c\u0435\u0440\u044f\u0435\u0442 \u0443\u0441\u043b\u043e\u0432\u0438\u044f \u0432 \u043c\u0435\u0441\u0442\u0435 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0438 ESP32/\u0438\u043d\u0432\u0435\u0440\u0442\u043e\u0440\u0430, " +
      "\u043d\u0435 \u0441\u0432\u044f\u0437\u0430\u043d \u0441 Modbus. \u041f\u0443\u0431\u043b\u0438\u043a\u0443\u0435\u0442\u0441\u044f \u043a\u0430\u043a AmbientTemperature/AmbientHumidity.",
    card_history_settings: "\u041b\u043e\u043a\u0430\u043b\u044c\u043d\u0430\u044f \u0438\u0441\u0442\u043e\u0440\u0438\u044f",
    label_history_enabled: "\u0412\u0435\u0441\u0442\u0438 \u0438\u0441\u0442\u043e\u0440\u0438\u044e (\u0434\u043b\u044f \u0442\u0430\u0431\u043b\u0438\u0446\u044b/\u0433\u0440\u0430\u0444\u0438\u043a\u0430 \u043d\u0438\u0436\u0435)",
    label_history_hours: "\u0413\u043b\u0443\u0431\u0438\u043d\u0430 \u0438\u0441\u0442\u043e\u0440\u0438\u0438, \u0447\u0430\u0441\u043e\u0432",
    hint_history: "\u041a\u043e\u043f\u0438\u0442\u0441\u044f \u0432 PSRAM \u043d\u0435\u0437\u0430\u0432\u0438\u0441\u0438\u043c\u043e \u043e\u0442 MQTT/WiFi. \u0412\u0440\u0435\u043c\u044f -- \u043f\u043e NTP.",
    card_flash_history: "\u041f\u0435\u0440\u0441\u0438\u0441\u0442\u0435\u043d\u0442\u043d\u0430\u044f \u0438\u0441\u0442\u043e\u0440\u0438\u044f (\u0444\u043b\u0435\u0448, \u043f\u0435\u0440\u0435\u0436\u0438\u0432\u0430\u0435\u0442 \u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0443)",
    label_flash_history_enabled: "\u0412\u043a\u043b\u044e\u0447\u0435\u043d\u0430",
    label_flash_history_interval: "\u0418\u043d\u0442\u0435\u0440\u0432\u0430\u043b \u0441\u043d\u0438\u043c\u043a\u043e\u0432, \u043c\u0438\u043d\u0443\u0442",
    hint_flash_history: "\u0412 \u043e\u0442\u043b\u0438\u0447\u0438\u0435 \u043e\u0442 \u0438\u0441\u0442\u043e\u0440\u0438\u0438 \u0432 PSRAM \u0432\u044b\u0448\u0435 (\u043a\u0430\u0436\u0434\u043e\u0435 \u0447\u0442\u0435\u043d\u0438\u0435, \u0442\u0435\u0440\u044f\u0435\u0442\u0441\u044f \u043f\u0440\u0438 " +
      "\u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0435), \u0441\u044e\u0434\u0430 \u0440\u0430\u0437 \u0432 N \u043c\u0438\u043d\u0443\u0442 \u043f\u0438\u0448\u0435\u0442\u0441\u044f \u043e\u0434\u0438\u043d \u0441\u043d\u0438\u043c\u043e\u043a " +
      "\u043f\u043e\u0441\u043b\u0435\u0434\u043d\u0438\u0445 \u0437\u043d\u0430\u0447\u0435\u043d\u0438\u0439 \u0432\u0441\u0435\u0445 \u0440\u0435\u0433\u0438\u0441\u0442\u0440\u043e\u0432 \u0432\u043e \u0444\u043b\u0435\u0448 -- \u0433\u0440\u0443\u0431\u0435\u0435, " +
      "\u043d\u043e \u043f\u0435\u0440\u0435\u0436\u0438\u0432\u0430\u0435\u0442 \u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0443/\u043f\u0435\u0440\u0435\u043f\u0440\u043e\u0448\u0438\u0432\u043a\u0443. \u0412\u044b\u043a\u043b\u044e\u0447\u0435\u043d\u043e " +
      "\u043f\u043e \u0443\u043c\u043e\u043b\u0447\u0430\u043d\u0438\u044e -- \u0443 \u0444\u043b\u0435\u0448-\u043f\u0430\u043c\u044f\u0442\u0438 \u043e\u0433\u0440\u0430\u043d\u0438\u0447\u0435\u043d\u043d\u044b\u0439 \u0440\u0435\u0441\u0443\u0440\u0441 " +
      "\u0446\u0438\u043a\u043b\u043e\u0432 \u0441\u0442\u0438\u0440\u0430\u043d\u0438\u044f, \u043f\u043e\u044d\u0442\u043e\u043c\u0443 \u043f\u0438\u0448\u0435\u043c \u0441\u043d\u0438\u043c\u043a\u0430\u043c\u0438, \u0430 \u043d\u0435 \u043a\u0430\u0436\u0434\u043e\u0435 " +
      "\u043e\u0442\u0434\u0435\u043b\u044c\u043d\u043e\u0435 \u0447\u0442\u0435\u043d\u0438\u0435. \u041f\u0440\u0438 \u0438\u043d\u0442\u0435\u0440\u0432\u0430\u043b\u0435 \u043f\u043e \u0443\u043c\u043e\u043b\u0447\u0430\u043d\u0438\u044e " +
      "(5 \u043c\u0438\u043d\u0443\u0442) \u0438\u0437\u043d\u043e\u0441 \u043f\u0440\u0435\u043d\u0435\u0431\u0440\u0435\u0436\u0438\u043c \u0437\u0430 \u0432\u0435\u0441\u044c \u0440\u0435\u0430\u043b\u044c\u043d\u044b\u0439 \u0441\u0440\u043e\u043a \u0441\u043b\u0443\u0436\u0431\u044b \u0443\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u0430.",
    link_flash_csv_export: "\u2193 \u0421\u043a\u0430\u0447\u0430\u0442\u044c \u043f\u0435\u0440\u0441\u0438\u0441\u0442\u0435\u043d\u0442\u043d\u0443\u044e \u0438\u0441\u0442\u043e\u0440\u0438\u044e \u0432 CSV",
    btn_save: "\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c \u0438 \u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c",
    btn_cancel: "\u041e\u0442\u043c\u0435\u043d\u0438\u0442\u044c \u0438\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u044f",
    card_history_view: "\u0418\u0441\u0442\u043e\u0440\u0438\u044f: \u0442\u0430\u0431\u043b\u0438\u0446\u0430 \u0438 \u0433\u0440\u0430\u0444\u0438\u043a",
    label_register: "\u0420\u0435\u0433\u0438\u0441\u0442\u0440",
    label_points: "\u0422\u043e\u0447\u0435\u043a (\u043c\u0430\u043a\u0441. 2000)",
    btn_load: "\u0417\u0430\u0433\u0440\u0443\u0437\u0438\u0442\u044c",
    link_csv_export: "\u2193 \u0421\u043a\u0430\u0447\u0430\u0442\u044c \u0432\u0441\u044e \u0438\u0441\u0442\u043e\u0440\u0438\u044e \u0432 CSV",
    th_time: "\u0412\u0440\u0435\u043c\u044f",
    th_value: "\u0417\u043d\u0430\u0447\u0435\u043d\u0438\u0435",
    card_logs: "\u041b\u043e\u0433\u0438 (live)",
    placeholder_log_filter: "\u0424\u0438\u043b\u044c\u0442\u0440 (\u043d\u0430\u043f\u0440\u0438\u043c\u0435\u0440: usb, modbus, error)",
    status_loaded: "\u041a\u043e\u043d\u0444\u0438\u0433 \u0437\u0430\u0433\u0440\u0443\u0436\u0435\u043d",
    status_json_error: "\u041e\u0448\u0438\u0431\u043a\u0430 \u0432 JSON \u0440\u0435\u0433\u0438\u0441\u0442\u0440\u043e\u0432: ",
    status_saved: "\u0421\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e, \u043f\u0435\u0440\u0435\u0437\u0430\u0433\u0440\u0443\u0437\u043a\u0430...",
    status_save_error: "\u041e\u0448\u0438\u0431\u043a\u0430 \u0441\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u0438\u044f: ",
    text_not_enough_data: "\u041d\u0435\u0434\u043e\u0441\u0442\u0430\u0442\u043e\u0447\u043d\u043e \u0434\u0430\u043d\u043d\u044b\u0445 \u0434\u043b\u044f \u0433\u0440\u0430\u0444\u0438\u043a\u0430",
    text_time_not_synced: "(\u0432\u0440\u0435\u043c\u044f \u043d\u0435 \u0441\u0438\u043d\u0445\u0440\u043e\u043d\u0438\u0437\u0438\u0440\u043e\u0432\u0430\u043d\u043e)",
    text_ntp_warning: "\u0432\u0440\u0435\u043c\u044f \u043d\u0435 \u0441\u0438\u043d\u0445\u0440\u043e\u043d\u0438\u0437\u0438\u0440\u043e\u0432\u0430\u043d\u043e (NTP)"
  }
};

let currentLang = localStorage.getItem('lang') || (navigator.language.startsWith('ru') ? 'ru' : 'en');

function applyLang(lang) {
  currentLang = lang;
  localStorage.setItem('lang', lang);
  document.documentElement.lang = lang;
  document.getElementById('lang_toggle').textContent = lang === 'en' ? 'RU' : 'EN';
  document.querySelectorAll('[data-i18n]').forEach(el => {
    const key = el.getAttribute('data-i18n');
    if (I18N[lang][key] !== undefined) el.textContent = I18N[lang][key];
  });
  document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
    const key = el.getAttribute('data-i18n-placeholder');
    if (I18N[lang][key] !== undefined) el.placeholder = I18N[lang][key];
  });
}

function toggleLang() {
  applyLang(currentLang === 'en' ? 'ru' : 'en');
}

async function loadConfig() {
  const r = await fetch('/api/config');
  const c = await r.json();
  wifi_ssid.value = c.wifi.ssid || '';
  wifi_password.value = c.wifi.password || '';
  mqtt_host.value = c.mqtt.host || '';
  mqtt_port.value = c.mqtt.port || 1883;
  mqtt_user.value = c.mqtt.user || '';
  mqtt_password.value = c.mqtt.password || '';
  mqtt_client_id.value = c.mqtt.client_id || '';
  mqtt_topic_prefix.value = c.mqtt.topic_prefix || '';
  mqtt_ha_discovery.checked = !!c.mqtt.ha_discovery;
  mqtt_tls.checked = !!c.mqtt.tls;
  mb_slave.value = c.modbus.slave_id;
  mb_baud.value = c.modbus.uart_baud;
  mb_interval.value = c.modbus.poll_interval_ms;
  mb_rx.value = c.modbus.uart_rx_pin;
  mb_tx.value = c.modbus.uart_tx_pin;
  mb_de.value = c.modbus.uart_de_pin;
  mb_registers.value = JSON.stringify(c.modbus.registers || [], null, 2);
  ota_password.value = c.ota.password || '';
  web_username.value = (c.web && c.web.username) || 'admin';
  web_password.value = (c.web && c.web.password) || '';
  rgb_led_enabled.checked = !!c.rgb_led.enabled;
  rgb_led_pin.value = c.rgb_led.pin;
  rgb_led_brightness.value = c.rgb_led.brightness;
  sht41_enabled.checked = !!(c.sht41 && c.sht41.enabled);
  sht41_sda_pin.value = (c.sht41 && c.sht41.sda_pin) || 8;
  sht41_scl_pin.value = (c.sht41 && c.sht41.scl_pin) || 9;
  history_enabled.checked = !!(c.history && c.history.enabled);
  history_hours.value = (c.history && c.history.hours) || 24;
  flash_history_enabled.checked = !!(c.flash_history && c.flash_history.enabled);
  flash_history_interval.value = (c.flash_history && c.flash_history.interval_min) || 5;

  const sel = document.getElementById('history_register');
  const prevSelected = sel.value;
  sel.innerHTML = '';
  (c.modbus.registers || []).forEach(r => {
    const opt = document.createElement('option');
    opt.value = r.name;
    opt.textContent = r.name + (r.unit ? ' (' + r.unit + ')' : '');
    sel.appendChild(opt);
  });
  if (prevSelected) sel.value = prevSelected;

  setStatus(I18N[currentLang].status_loaded, 'ok');
}

function setStatus(msg, cls) {
  const el = document.getElementById('status');
  el.textContent = msg;
  el.className = cls || '';
}

async function saveConfig() {
  let registers;
  try {
    registers = JSON.parse(mb_registers.value);
  } catch (e) {
    setStatus(I18N[currentLang].status_json_error + e.message, 'err');
    return;
  }

  const payload = {
    wifi: { ssid: wifi_ssid.value, password: wifi_password.value },
    mqtt: {
      host: mqtt_host.value, port: parseInt(mqtt_port.value) || 1883,
      user: mqtt_user.value, password: mqtt_password.value,
      client_id: mqtt_client_id.value, topic_prefix: mqtt_topic_prefix.value,
      ha_discovery: mqtt_ha_discovery.checked, tls: mqtt_tls.checked
    },
    ota: { password: ota_password.value },
    web: { username: web_username.value, password: web_password.value },
    history: {
      enabled: history_enabled.checked,
      hours: parseFloat(history_hours.value)
    },
    flash_history: {
      enabled: flash_history_enabled.checked,
      interval_min: parseInt(flash_history_interval.value)
    },
    rgb_led: {
      enabled: rgb_led_enabled.checked,
      pin: parseInt(rgb_led_pin.value),
      brightness: parseInt(rgb_led_brightness.value)
    },
    sht41: {
      enabled: sht41_enabled.checked,
      sda_pin: parseInt(sht41_sda_pin.value),
      scl_pin: parseInt(sht41_scl_pin.value)
    },
    modbus: {
      uart_rx_pin: parseInt(mb_rx.value), uart_tx_pin: parseInt(mb_tx.value),
      uart_de_pin: parseInt(mb_de.value), uart_baud: parseInt(mb_baud.value),
      slave_id: parseInt(mb_slave.value), poll_interval_ms: parseInt(mb_interval.value),
      registers: registers
    }
  };

  const r = await fetch('/api/config', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify(payload)
  });
  if (r.ok) {
    setStatus(I18N[currentLang].status_saved, 'ok');
    setTimeout(() => location.reload(), 4000);
  } else {
    const t = await r.text();
    setStatus(I18N[currentLang].status_save_error + t, 'err');
  }
}

async function loadHistory() {
  const reg = document.getElementById('history_register').value;
  if (!reg) { return; }
  const points = document.getElementById('history_points').value || 300;

  const r = await fetch('/api/history?register=' + encodeURIComponent(reg) + '&points=' + points);
  const data = await r.json();
  const pts = data.points || [];

  // -- table (most recent first) --
  const tbody = document.getElementById('history_table_body');
  tbody.innerHTML = '';
  const warnOldDates = pts.length && pts[0].t < 1600000000; // before Sep 2020 -- clearly no NTP yet
  for (let i = pts.length - 1; i >= 0; i--) {
    const tr = document.createElement('tr');
    const d = new Date(pts[i].t * 1000);
    const tCell = document.createElement('td');
    tCell.style.padding = '4px 8px';
    tCell.textContent = pts[i].t < 1600000000 ? ('#' + i + ' ' + I18N[currentLang].text_time_not_synced) : d.toLocaleString();
    const vCell = document.createElement('td');
    vCell.style.padding = '4px 8px';
    vCell.textContent = pts[i].v;
    tr.appendChild(tCell); tr.appendChild(vCell);
    tbody.appendChild(tr);
  }

  // -- graph (simple canvas line, no external libraries) --
  const canvas = document.getElementById('history_canvas');
  const ctx = canvas.getContext('2d');
  canvas.width = canvas.clientWidth; // match the block's real width
  const W = canvas.width, H = canvas.height, PAD = 30;
  ctx.clearRect(0, 0, W, H);

  if (pts.length < 2) {
    ctx.fillStyle = '#8b93a3';
    ctx.font = '13px sans-serif';
    ctx.fillText(I18N[currentLang].text_not_enough_data, 10, H / 2);
    return;
  }

  const values = pts.map(p => p.v);
  const minV = Math.min(...values), maxV = Math.max(...values);
  const range = (maxV - minV) || 1;

  ctx.strokeStyle = '#4f8cff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  pts.forEach((p, i) => {
    const x = PAD + (i / (pts.length - 1)) * (W - PAD * 2);
    const y = H - PAD - ((p.v - minV) / range) * (H - PAD * 2);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();

  ctx.fillStyle = '#8b93a3';
  ctx.font = '11px sans-serif';
  ctx.fillText(maxV.toFixed(2), 4, PAD);
  ctx.fillText(minV.toFixed(2), 4, H - PAD + 4);
  if (warnOldDates) {
    ctx.fillStyle = '#ff5d5d';
    ctx.fillText(I18N[currentLang].text_ntp_warning, W - 190, 14);
  }
}

const logEl = document.getElementById('log');
let logCursor = 0;

let allLogLines = [];

function renderLogs() {
  const filter = document.getElementById('log_filter').value.toLowerCase();
  const atBottom = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 10;
  const filtered = filter ? allLogLines.filter(l => l.toLowerCase().includes(filter)) : allLogLines;
  logEl.textContent = filtered.join("\n");
  if (atBottom) logEl.scrollTop = logEl.scrollHeight;
}
document.getElementById('log_filter').addEventListener('input', renderLogs);

async function pollLogs() {
  try {
    const r = await fetch('/api/logs?since=' + logCursor);
    const data = await r.json();
    if (data.lines && data.lines.length) {
      allLogLines.push(...data.lines);
      if (allLogLines.length > 2000) {
        allLogLines = allLogLines.slice(-1500);
      }
      renderLogs();
    }
    logCursor = data.next;
  } catch (e) { /* server busy/rebooting -- just try again next time */ }
}
setInterval(pollLogs, 1500);
pollLogs();

async function loadVersion() {
  try {
    const r = await fetch('/api/version');
    const v = await r.json();
    document.getElementById('fw_version').textContent = 'v' + v.version + ' (' + v.build + ')';
  } catch (e) { /* not critical, just skip showing the version */ }
}
loadVersion();

applyLang(currentLang);
loadConfig();
</script>
</body>
</html>
)HTML";
