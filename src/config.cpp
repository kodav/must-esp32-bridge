#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

AppConfig g_config;

static const char *CONFIG_PATH = "/config.json";

void configSetDefaults() {
  g_config = AppConfig(); // структура уже содержит разумные значения по умолчанию

  // Регистры по умолчанию -- то, что уже подтверждено дампом с реального
  // инвертора (slave 4, старт 25200). Массив специально маленький и с
  // "сырыми" (scale=1) значениями там, где физический смысл ещё не до конца
  // ясен -- поправьте scale/unit по мере уточнения карты регистров.
  // Полная карта регистров, взятая из открытой Home Assistant интеграции
  // mukaschultze/ha-must-inverter (MIT), протестированной именно на PV18-3024 VPM
  // (aka "PV1800" в их терминологии). Адреса и масштабы -- не догадки, а
  // готовая расшифрованная карта из const.py этого проекта.
  //
  // is_signed выставлен вручную для полей, которые физически могут быть
  // отрицательными (реактивная мощность, ток/мощность батареи, температуры) --
  // в исходном const.py это не указано явно, значения выбраны по смыслу поля.
  //
  // count=2 -- для накопительных счётчиков энергии (AccumulatedXxxPower и т.п.),
  // вычислено по разнице соседних адресов в оригинальной таблице (32-битные поля).
  g_config.registers = {
    {"AbsorbVoltage", "Напряжение заряда (Absorb)", 10102, 1, false, 0.1f, "V", false},
    {"FloatVoltage", "Напряжение поддержки (Float)", 10103, 1, false, 0.1f, "V", true, 24.0f, 29.2f, 0.1f},
    {"AbsorptionVoltage", "Напряжение абсорбции", 10104, 1, false, 0.1f, "V", true, 24.0f, 29.2f, 0.1f},
    {"BatteryLowVoltage", "Батарея: нижний порог напряжения", 10105, 1, false, 0.1f, "V", true, 20.0f, 24.0f, 0.1f},
    {"BatteryHighVoltage", "Батарея: верхний порог напряжения", 10107, 1, false, 0.1f, "V", false},
    {"MaxChargerCurrent", "Макс. ток заряда", 10108, 1, false, 0.1f, "A", true, 1.0f, 100.0f, 1.0f},
    {"AbsorbChargerCurrent", "Ток заряда (Absorb)", 10109, 1, false, 0.1f, "A", false},
    {"BatteryAh", "Ёмкость батареи", 10111, 1, false, 1.0f, "Ah", true, 1.0f, 900.0f, 1.0f},
    {"BatteryEqualizationVoltage", "Напряжение выравнивания батареи", 10119, 1, false, 0.1f, "V", true, 24.0f, 29.2f, 0.1f},
    {"PvVoltage", "Напряжение солнечных панелей", 15205, 1, false, 0.1f, "V", false},
    {"BatteryVoltage", "Напряжение батареи (контроллер заряда)", 15206, 1, false, 0.1f, "V", false},
    {"ChargerCurrent", "Ток заряда от солнца", 15207, 1, false, 0.1f, "A", false},
    {"ChargerPower", "Мощность заряда от солнца", 15208, 1, false, 1.0f, "W", false},
    {"RadiatorTemperature", "Температура радиатора (контроллер)", 15209, 1, true, 1.0f, "°C", false},
    {"ExternalTemperature", "Температура внешнего датчика", 15210, 1, true, 1.0f, "°C", false},
    {"RatedCurrent", "Номинальный ток", 15216, 1, false, 0.1f, "A", false},
    {"AccumulatedPower", "Накопленная энергия (заряд)", 15217, 2, false, 1.0f, "kWh", false},
    {"AccumulatedTime", "Наработка", 15219, 2, false, 1.0f, "s", false},
    {"InverterOutputVoltageSet", "Уставка выходного напряжения", 20102, 1, false, 0.1f, "V", true, 220.0f, 240.0f, 1.0f},
    {"InverterOutputFrequencySet", "Уставка выходной частоты", 20103, 1, false, 0.01f, "Hz", true, 50.0f, 60.0f, 10.0f},
    {"InverterMaxDischargerCurrent", "Макс. ток разряда батареи", 20113, 1, false, 0.1f, "A", true, 1.0f, 13.0f, 0.1f},
    {"BatteryStopDischargingVoltage", "Напряжение остановки разряда", 20118, 1, false, 0.1f, "V", true, 22.0f, 29.0f, 0.1f},
    {"BatteryStopChargingVoltage", "Напряжение остановки заряда", 20119, 1, false, 0.1f, "V", true, 22.0f, 29.0f, 0.1f},
    {"GridMaxChargerCurrentSet", "Уставка макс. тока заряда от сети", 20125, 1, false, 0.1f, "A", true, 20.0f, 30.0f, 10.0f},
    {"InverterBatteryLowVoltage", "Батарея: нижний порог (инвертор)", 20127, 1, false, 0.1f, "V", true, 20.0f, 24.0f, 0.1f},
    {"InverterBatteryHighVoltage", "Батарея: верхний порог (инвертор)", 20128, 1, false, 0.1f, "V", false},
    {"MaxCombineChargerCurrent", "Макс. суммарный ток заряда", 20132, 1, false, 0.1f, "A", true, 1.0f, 80.0f, 1.0f},
    {"AcVoltageGrade", "Номинальное напряжение сети", 25202, 1, false, 1.0f, "V", false},
    {"RatedPower", "Номинальная мощность (VA)", 25203, 1, false, 1.0f, "VA", false},
    {"InverterBatteryVoltage", "Напряжение батареи", 25205, 1, false, 0.1f, "V", false},
    {"InverterVoltage", "Напряжение на выходе инвертора", 25206, 1, false, 0.1f, "V", false},
    {"GridVoltage", "Напряжение сети", 25207, 1, false, 0.1f, "V", false},
    {"BusVoltage", "Напряжение шины (DC bus)", 25208, 1, false, 0.1f, "V", false},
    {"ControlCurrent", "Контрольный ток", 25209, 1, false, 0.1f, "A", false},
    {"InverterCurrent", "Ток инвертора", 25210, 1, false, 0.1f, "A", false},
    {"GridCurrent", "Ток сети", 25211, 1, false, 0.1f, "A", false},
    {"LoadCurrent", "Ток нагрузки", 25212, 1, false, 0.1f, "A", false},
    {"PInverter", "Активная мощность инвертора", 25213, 1, false, 1.0f, "W", false},
    {"PGrid", "Активная мощность сети", 25214, 1, false, 1.0f, "W", false},
    {"PLoad", "Активная мощность нагрузки", 25215, 1, false, 1.0f, "W", false},
    {"LoadPercent", "Загрузка инвертора", 25216, 1, false, 1.0f, "%", false},
    {"SInverter", "Полная мощность инвертора", 25217, 1, false, 1.0f, "VA", false},
    {"SGrid", "Полная мощность сети", 25218, 1, false, 1.0f, "VA", false},
    {"Sload", "Полная мощность нагрузки", 25219, 1, false, 1.0f, "VA", false},
    {"Qinverter", "Реактивная мощность инвертора", 25221, 1, true, 1.0f, "var", false},
    {"Qgrid", "Реактивная мощность сети", 25222, 1, true, 1.0f, "var", false},
    {"Qload", "Реактивная мощность нагрузки", 25223, 1, true, 1.0f, "var", false},
    {"InverterFrequency", "Частота инвертора", 25225, 1, false, 0.01f, "Hz", false},
    {"GridFrequency", "Частота сети", 25226, 1, false, 0.01f, "Hz", false},
    {"AcRadiatorTemperature", "Температура радиатора (AC)", 25233, 1, true, 1.0f, "°C", false},
    {"TransformerTemperature", "Температура трансформатора", 25234, 1, true, 1.0f, "°C", false},
    {"DcRadiatorTemperature", "Температура радиатора (DC)", 25235, 1, true, 1.0f, "°C", false},
    {"AccumulatedChargerPower", "Накоплено: заряд от солнца", 25245, 2, false, 1.0f, "kWh", false},
    {"AccumulatedDischargerPower", "Накоплено: разряд батареи", 25247, 2, false, 1.0f, "kWh", false},
    {"AccumulatedBuyPower", "Накоплено: куплено у сети", 25249, 2, false, 1.0f, "kWh", false},
    {"AccumulatedSellPower", "Накоплено: продано в сеть", 25251, 2, false, 1.0f, "kWh", false},
    {"AccumulatedLoadPower", "Накоплено: потреблено нагрузкой", 25253, 2, false, 1.0f, "kWh", false},
    {"AccumulatedSelfUsePower", "Накоплено: собственное потребление", 25255, 2, false, 1.0f, "kWh", false},
    {"AccumulatedPvSellPower", "Накоплено: продано от солнца", 25257, 2, false, 1.0f, "kWh", false},
    {"AccumulatedGridChargerPower", "Накоплено: заряд от сети", 25259, 2, false, 1.0f, "kWh", false},
    {"BattPower", "Мощность батареи", 25273, 1, true, 1.0f, "W", false},
    {"BattCurrent", "Ток батареи", 25274, 1, true, 1.0f, "A", false},
    {"RatedPowerW", "Номинальная мощность (Вт)", 25277, 1, false, 1.0f, "W", false},

    // Локальный датчик SHT41 (I2C, не Modbus) -- см. temp_humidity.cpp.
    // address/count не используются (is_local=true), значения приходят
    // напрямую из модуля датчика.
    {"AmbientTemperature", "Температура у инвертора", 0, 1, false, 1.0f, "°C", false, 0.0f, 0.0f, 0.0f, false, true},
    {"AmbientHumidity", "Влажность у инвертора", 0, 1, false, 1.0f, "%", false, 0.0f, 0.0f, 0.0f, false, true},
  };
}

static bool ensureFs() {
  if (!LittleFS.begin(false)) {
    Serial.println("[config] LittleFS mount failed, formatting...");
    if (!LittleFS.begin(true)) {
      Serial.println("[config] LittleFS format failed!");
      return false;
    }
  }
  return true;
}

String configToJson() {
  JsonDocument doc;

  doc["wifi"]["ssid"] = g_config.wifi_ssid;
  doc["wifi"]["password"] = g_config.wifi_password;

  doc["mqtt"]["host"] = g_config.mqtt_host;
  doc["mqtt"]["port"] = g_config.mqtt_port;
  doc["mqtt"]["user"] = g_config.mqtt_user;
  doc["mqtt"]["password"] = g_config.mqtt_password;
  doc["mqtt"]["client_id"] = g_config.mqtt_client_id;
  doc["mqtt"]["topic_prefix"] = g_config.mqtt_topic_prefix;
  doc["mqtt"]["ha_discovery"] = g_config.mqtt_ha_discovery;
  doc["mqtt"]["tls"] = g_config.mqtt_tls;

  doc["history"]["enabled"] = g_config.history_enabled;
  doc["history"]["hours"] = g_config.history_hours;

  doc["ota"]["password"] = g_config.ota_password;
  doc["web"]["username"] = g_config.web_username;
  doc["web"]["password"] = g_config.web_password;

  doc["rgb_led"]["enabled"] = g_config.rgb_led_enabled;
  doc["rgb_led"]["pin"] = g_config.rgb_led_pin;
  doc["rgb_led"]["brightness"] = g_config.rgb_led_brightness;
  doc["sht41"]["enabled"] = g_config.sht41_enabled;
  doc["sht41"]["sda_pin"] = g_config.sht41_sda_pin;
  doc["sht41"]["scl_pin"] = g_config.sht41_scl_pin;

  JsonObject mb = doc["modbus"].to<JsonObject>();
  mb["uart_rx_pin"] = g_config.uart_rx_pin;
  mb["uart_tx_pin"] = g_config.uart_tx_pin;
  mb["uart_de_pin"] = g_config.uart_de_pin;
  mb["uart_baud"] = g_config.uart_baud;
  mb["slave_id"] = g_config.slave_id;
  mb["poll_interval_ms"] = g_config.poll_interval_ms;

  JsonArray regs = mb["registers"].to<JsonArray>();
  for (auto &r : g_config.registers) {
    JsonObject o = regs.add<JsonObject>();
    o["name"] = r.name;
    o["name_ru"] = r.name_ru;
    o["address"] = r.address;
    o["count"] = r.count;
    o["is_signed"] = r.is_signed;
    o["scale"] = r.scale;
    o["unit"] = r.unit;
    o["writable"] = r.writable;
    if (r.writable) {
      o["min"] = r.min_value;
      o["max"] = r.max_value;
      o["step"] = r.step;
      o["is_switch"] = r.is_switch;
    }
    o["is_local"] = r.is_local;
    if (!r.enum_map.empty()) {
      JsonObject em = o["enum_map"].to<JsonObject>();
      for (auto &kv : r.enum_map) {
        em[String(kv.first)] = kv.second;
      }
    }
    if (!r.bit_map.empty()) {
      JsonObject bm = o["bit_map"].to<JsonObject>();
      for (auto &kv : r.bit_map) {
        bm[String(kv.first)] = kv.second;
      }
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

bool configFromJson(const String &json, String &errorOut) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    errorOut = String("JSON parse error: ") + err.c_str();
    return false;
  }

  AppConfig c; // собираем во временную структуру, чтобы не сломать текущий конфиг при ошибке

  c.wifi_ssid = doc["wifi"]["ssid"] | "";
  c.wifi_password = doc["wifi"]["password"] | "";

  c.mqtt_host = doc["mqtt"]["host"] | "";
  c.mqtt_port = doc["mqtt"]["port"] | 1883;
  c.mqtt_user = doc["mqtt"]["user"] | "";
  c.mqtt_password = doc["mqtt"]["password"] | "";
  c.mqtt_client_id = doc["mqtt"]["client_id"] | "must-pv18-esp32";
  c.mqtt_topic_prefix = doc["mqtt"]["topic_prefix"] | "must_pv18";
  c.mqtt_ha_discovery = doc["mqtt"]["ha_discovery"] | true;
  c.mqtt_tls = doc["mqtt"]["tls"] | false;

  c.history_enabled = doc["history"]["enabled"] | true;
  c.history_hours = doc["history"]["hours"] | 24.0f;

  c.ota_password = doc["ota"]["password"] | "";
  c.web_username = (const char*)(doc["web"]["username"] | "admin");
  c.web_password = (const char*)(doc["web"]["password"] | "");

  c.rgb_led_enabled = doc["rgb_led"]["enabled"] | true;
  c.rgb_led_pin = doc["rgb_led"]["pin"] | 48;
  c.rgb_led_brightness = doc["rgb_led"]["brightness"] | 40;
  c.sht41_enabled = doc["sht41"]["enabled"] | false;
  c.sht41_sda_pin = doc["sht41"]["sda_pin"] | 8;
  c.sht41_scl_pin = doc["sht41"]["scl_pin"] | 9;

  JsonObject mb = doc["modbus"];
  if (mb.isNull()) {
    errorOut = "missing 'modbus' section";
    return false;
  }
  c.uart_rx_pin = mb["uart_rx_pin"] | 16;
  c.uart_tx_pin = mb["uart_tx_pin"] | 17;
  c.uart_de_pin = mb["uart_de_pin"] | -1;
  c.uart_baud = mb["uart_baud"] | 19200;
  c.slave_id = mb["slave_id"] | 4;
  c.poll_interval_ms = mb["poll_interval_ms"] | 5000;

  JsonArray regs = mb["registers"];
  if (regs.isNull()) {
    errorOut = "missing 'modbus.registers' array";
    return false;
  }
  c.registers.clear();
  for (JsonObject o : regs) {
    RegisterDef r;
    r.name = (const char*)(o["name"] | "");
    r.name_ru = (const char*)(o["name_ru"] | "");
    r.address = o["address"] | 0;
    r.count = o["count"] | 1;
    r.is_signed = o["is_signed"] | false;
    r.scale = o["scale"] | 1.0f;
    r.unit = (const char*)(o["unit"] | "");
    r.writable = o["writable"] | false;
    r.min_value = o["min"] | 0.0f;
    r.max_value = o["max"] | 0.0f;
    r.step = o["step"] | 0.0f;
    r.is_switch = o["is_switch"] | false;
    r.is_local = o["is_local"] | false;
    r.enum_map.clear();
    JsonObject em = o["enum_map"];
    if (!em.isNull()) {
      for (JsonPair kv : em) {
        int code = atoi(kv.key().c_str());
        String text = (const char*)(kv.value() | "");
        r.enum_map.push_back({code, text});
      }
    }
    r.bit_map.clear();
    JsonObject bm = o["bit_map"];
    if (!bm.isNull()) {
      for (JsonPair kv : bm) {
        int bit = atoi(kv.key().c_str());
        String text = (const char*)(kv.value() | "");
        r.bit_map.push_back({bit, text});
      }
    }
    if (r.name.length() == 0) {
      errorOut = "register with empty name";
      return false;
    }
    c.registers.push_back(r);
  }

  g_config = c;
  return true;
}

bool configLoad() {
  configSetDefaults();
  if (!ensureFs()) return false;

  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("[config] no config.json yet, using defaults");
    return true; // не ошибка -- первый запуск
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println("[config] failed to open config.json");
    return false;
  }
  String content = f.readString();
  f.close();

  String err;
  if (!configFromJson(content, err)) {
    Serial.printf("[config] failed to parse config.json: %s\n", err.c_str());
    configSetDefaults();
    return false;
  }
  Serial.println("[config] loaded OK");
  return true;
}

bool configSave() {
  if (!ensureFs()) return false;
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println("[config] failed to open config.json for write");
    return false;
  }
  String json = configToJson();
  f.print(json);
  f.close();
  Serial.println("[config] saved OK");
  return true;
}
