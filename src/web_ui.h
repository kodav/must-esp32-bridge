#pragma once

// Встроенная веб-страница конфигурации + логов.
// Раздаётся напрямую из flash (PROGMEM), отдельная файловая система
// под статику не нужна -- вся прошивка это один .bin для OTA.

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ru">
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
           display:flex; align-items:center; justify-content:space-between; }
  header h1 { font-size: 16px; margin:0; font-weight:600; }
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
  <a class="link" href="/update">Прошивка (OTA) &rarr;</a>
</header>
<div class="wrap">

  <div class="card">
    <h2>WiFi</h2>
    <div class="row">
      <div><label>SSID</label><input id="wifi_ssid" type="text"></div>
      <div><label>Пароль</label><input id="wifi_password" type="password"></div>
    </div>
  </div>

  <div class="card">
    <h2>MQTT</h2>
    <div class="row">
      <div><label>Хост</label><input id="mqtt_host" type="text" placeholder="192.168.1.10"></div>
      <div><label>Порт</label><input id="mqtt_port" type="number" value="1883"></div>
    </div>
    <div class="row">
      <div><label>Пользователь</label><input id="mqtt_user" type="text"></div>
      <div><label>Пароль</label><input id="mqtt_password" type="password"></div>
    </div>
    <div class="row">
      <div><label>Client ID</label><input id="mqtt_client_id" type="text"></div>
      <div><label>Topic prefix</label><input id="mqtt_topic_prefix" type="text"></div>
    </div>
    <label><input id="mqtt_ha_discovery" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">Home Assistant MQTT Discovery</label>
    <br>
    <label><input id="mqtt_tls" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">TLS (шифрование, порт обычно 8883)</label>
    <p class="hint">Включите, если MQTT-порт открыт наружу в интернет
      (например, датчик на даче, брокер в городе через белый IP) --
      иначе логин/пароль от MQTT идут открытым текстом через весь путь.
      Без TLS используйте только в пределах локальной сети.</p>
  </div>

  <div class="card">
    <h2>Modbus / RS485</h2>
    <div class="row">
      <div><label>Slave ID</label><input id="mb_slave" type="number" value="4"></div>
      <div><label>Baud rate</label><input id="mb_baud" type="number" value="19200"></div>
    </div>
    <div class="row">
      <div><label>Интервал опроса, мс</label><input id="mb_interval" type="number" value="5000"></div>
    </div>
    <div class="row">
      <div><label>UART RX pin</label><input id="mb_rx" type="number" value="16"></div>
      <div><label>UART TX pin</label><input id="mb_tx" type="number" value="17"></div>
      <div><label>DE pin (-1 если не нужен)</label><input id="mb_de" type="number" value="-1"></div>
    </div>
    <p class="hint">DE pin нужен только для RS485-модулей без автонаправления
      (без авточипа вроде MAX13487). Для большинства готовых модулей
      "RS485-TTL авто" оставьте -1.</p>

    <label>Регистры для опроса (JSON-массив)</label>
    <textarea id="mb_registers" spellcheck="false"></textarea>
    <p class="hint">Поля: name (техническое, латиницей -- используется в MQTT-топике
      и unique_id, не меняйте после первой настройки в HA, иначе появится
      дубль сущности), name_ru (отображаемое имя в Home Assistant, можно
      редактировать свободно в любой момент), address, count (1 или 2),
      is_signed, scale, unit, writable (true/false -- предлагать ли регистр
      в списке записи ниже; по умолчанию false, т.е. только чтение),
      enum_map (необязательно -- объект код→текст, например
      {"0":"Включение","4":"Bypass"} -- если указан, в MQTT/HA пойдёт
      текст вместо числа, device_class становится enum),
      bit_map (необязательно -- как enum_map, но ключ это номер БИТА, а
      не значения -- для битовых масок ошибок; может быть активно
      несколько бит сразу, в MQTT пойдёт список через запятую или "OK"),
      min/max/step (обязательны при writable:true и без enum_map -- диапазон
      для слайдера в Home Assistant, number-сущность вместо обычного сенсора),
      is_switch (true -- регистр становится HA switch вместо number, для
      булевых 0/1 параметров), enum_map + writable:true вместе --
      регистр становится HA select (выпадающий список) вместо сенсора.
      Подтверждено дампом реального инвертора: slave=4, function=3 (Read
      Holding Registers), рабочий диапазон начинается с адреса 25200.</p>
  </div>

  <div class="card">
    <h2>OTA</h2>
    <label>Пароль для /update (пусто = без пароля)</label>
    <input id="ota_password" type="password">
  </div>

  <div class="card">
    <h2>Доступ к веб-интерфейсу</h2>
    <div class="row">
      <div><label>Логин</label><input id="web_username" type="text"></div>
      <div><label>Пароль (пусто = без защиты)</label><input id="web_password" type="password"></div>
    </div>
    <p class="hint">HTTP Basic Auth на весь веб-интерфейс и API (кроме
      /update, у него отдельный пароль выше). Пусто по умолчанию --
      осторожно: как только зададите пароль здесь и сохраните, браузер
      запросит логин/пароль при следующей загрузке страницы -- не
      забудьте их, иначе придётся сбрасывать конфиг заново.</p>
  </div>

  <div class="card">
    <h2>RGB-индикатор</h2>
    <label><input id="rgb_led_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">Включён</label>
    <div class="row">
      <div><label>GPIO пин</label><input id="rgb_led_pin" type="number" value="48"></div>
      <div><label>Яркость (0-255)</label><input id="rgb_led_brightness" type="number" value="40"></div>
    </div>
    <p class="hint">Зелёный = всё ОК. Красный = инвертор не отвечает.
      Оранжевый = нет связи с MQTT. Синий = подключение к WiFi.
      Жёлтый = точка доступа для настройки. Фиолетовый = идёт OTA.</p>
  </div>

  <div class="card">
    <h2>Датчик температуры/влажности (SHT41, I2C)</h2>
    <label><input id="sht41_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">Включён</label>
    <div class="row">
      <div><label>SDA пин</label><input id="sht41_sda_pin" type="number" value="8"></div>
      <div><label>SCL пин</label><input id="sht41_scl_pin" type="number" value="9"></div>
    </div>
    <p class="hint">Измеряет условия в месте установки ESP32/инвертора,
      не связан с Modbus. Публикуется как AmbientTemperature/AmbientHumidity
      в тех же MQTT/истории, что и обычные регистры.</p>
  </div>

  <div class="card">
    <h2>Локальная история</h2>
    <label><input id="history_enabled" type="checkbox" style="width:auto;display:inline-block;margin-right:6px;">Вести историю (для таблицы/графика ниже)</label>
    <label>Глубина истории, часов</label>
    <input id="history_hours" type="number" value="24" step="0.5">
    <p class="hint">Копится в PSRAM независимо от MQTT/WiFi -- можно
      смотреть даже если MQTT/Home Assistant вообще не настроены. Реальная
      ёмкость в записях зависит от числа регистров и интервала опроса,
      точный расчёт увидите в логах после сохранения. Время -- по NTP,
      если WiFi ещё не успел засинхронизировать время, возможны
      неправдоподобные даты (около 1970 года) для самых первых записей.</p>
  </div>

  <button onclick="saveConfig()">Сохранить и перезагрузить</button>
  <button class="secondary" onclick="loadConfig()">Отменить изменения</button>
  <div id="status"></div>

  <div class="card" style="margin-top:24px;">
    <h2>История: таблица и график</h2>
    <div class="row">
      <div>
        <label>Регистр</label>
        <select id="history_register"></select>
      </div>
      <div>
        <label>Точек (макс. 2000)</label>
        <input id="history_points" type="number" value="300">
      </div>
    </div>
    <button onclick="loadHistory()">Загрузить</button>
    <canvas id="history_canvas" width="800" height="220" style="width:100%;height:220px;background:#0a0c10;border:1px solid var(--border);border-radius:8px;margin-top:12px;"></canvas>
    <div id="history_table_wrap" style="max-height:260px;overflow-y:auto;margin-top:12px;">
      <table style="width:100%;border-collapse:collapse;font-size:12px;">
        <thead><tr style="text-align:left;color:var(--muted);">
          <th style="padding:4px 8px;">Время</th><th style="padding:4px 8px;">Значение</th>
        </tr></thead>
        <tbody id="history_table_body"></tbody>
      </table>
    </div>
  </div>

  <div class="card" style="margin-top:24px;">
    <h2>Логи (live)</h2>
    <input id="log_filter" type="text" placeholder="Фильтр (например: usb, modbus, error)" style="margin-bottom:8px;">
    <div id="log"></div>
  </div>

</div>

<script>
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

  setStatus('Конфиг загружен', 'ok');
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
    setStatus('Ошибка в JSON регистров: ' + e.message, 'err');
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
    setStatus('Сохранено, перезагрузка...', 'ok');
    setTimeout(() => location.reload(), 4000);
  } else {
    const t = await r.text();
    setStatus('Ошибка сохранения: ' + t, 'err');
  }
}

async function loadHistory() {
  const reg = document.getElementById('history_register').value;
  if (!reg) { return; }
  const points = document.getElementById('history_points').value || 300;

  const r = await fetch('/api/history?register=' + encodeURIComponent(reg) + '&points=' + points);
  const data = await r.json();
  const pts = data.points || [];

  // -- таблица (последние сначала) --
  const tbody = document.getElementById('history_table_body');
  tbody.innerHTML = '';
  const warnOldDates = pts.length && pts[0].t < 1600000000; // раньше сен 2020 -- явно ещё нет NTP
  for (let i = pts.length - 1; i >= 0; i--) {
    const tr = document.createElement('tr');
    const d = new Date(pts[i].t * 1000);
    const tCell = document.createElement('td');
    tCell.style.padding = '4px 8px';
    tCell.textContent = pts[i].t < 1600000000 ? ('#' + i + ' (время не синхронизировано)') : d.toLocaleString();
    const vCell = document.createElement('td');
    vCell.style.padding = '4px 8px';
    vCell.textContent = pts[i].v;
    tr.appendChild(tCell); tr.appendChild(vCell);
    tbody.appendChild(tr);
  }

  // -- график (простая линия на canvas, без внешних библиотек) --
  const canvas = document.getElementById('history_canvas');
  const ctx = canvas.getContext('2d');
  canvas.width = canvas.clientWidth; // подстраиваемся под реальную ширину блока
  const W = canvas.width, H = canvas.height, PAD = 30;
  ctx.clearRect(0, 0, W, H);

  if (pts.length < 2) {
    ctx.fillStyle = '#8b93a3';
    ctx.font = '13px sans-serif';
    ctx.fillText('Недостаточно данных для графика', 10, H / 2);
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
    ctx.fillText('время не синхронизировано (NTP)', W - 190, 14);
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
  } catch (e) { /* сервер занят/перезагружается -- просто попробуем в следующий раз */ }
}
setInterval(pollLogs, 1500);
pollLogs();

async function loadVersion() {
  try {
    const r = await fetch('/api/version');
    const v = await r.json();
    document.getElementById('fw_version').textContent = 'v' + v.version + ' (' + v.build + ')';
  } catch (e) { /* не критично, просто не покажем версию */ }
}
loadVersion();

loadConfig();
</script>
</body>
</html>
)HTML";
