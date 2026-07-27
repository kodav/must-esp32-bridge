#include "mqtt_handler.h"
#include "config.h"
#include "log_buffer.h"
#include "version.h"
#include "modbus_poll.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

static WiFiClient s_plainClient;
static WiFiClientSecure s_secureClient;
static PubSubClient s_mqtt;
static bool s_clientBound = false;
static bool s_discoverySent = false;
static unsigned long s_lastReconnectAttempt = 0;

static String deviceObjectId() {
  return "must_pv18_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

static const RegisterDef *findRegister(const String &name) {
  for (auto &r : g_config.registers) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

// Расшифровывает значение enum-регистра в текст по enum_map. Если кода
// нет в карте (незнакомое/новое состояние) -- отдаёт хотя бы сырое число
// текстом, чтобы не потерять информацию молча.
static String lookupEnumText(const RegisterDef &r, float value) {
  int code = (int)lroundf(value);
  for (auto &kv : r.enum_map) {
    if (kv.first == code) return kv.second;
  }
  return String(code);
}

// Разбирает битовую маску: "OK" если raw==0, иначе список активных
// флагов через запятую. Установленный бит, которого нет в bit_map,
// добавляется как "bitN" -- чтобы не терять сигнал молча.
static String lookupBitmaskText(const RegisterDef &r, float value) {
  long raw = lroundf(value);
  if (raw == 0) return "OK";

  String result;
  for (int bit = 0; bit < 32; bit++) {
    if (!(raw & (1L << bit))) continue;
    String text;
    for (auto &kv : r.bit_map) {
      if (kv.first == bit) { text = kv.second; break; }
    }
    if (text.length() == 0) text = "bit" + String(bit);
    if (result.length()) result += ", ";
    result += text;
  }
  return result;
}

static String guessDeviceClass(const String &unit) {
  if (unit == "V") return "voltage";
  if (unit == "A") return "current";
  if (unit == "W") return "power";
  if (unit == "VA") return "apparent_power";
  if (unit == "var") return "reactive_power";
  if (unit == "%") return ""; // % -- это LoadPercent (загрузка), не заряд батареи;
                                // без явного device_class HA покажет как обычный %-сенсор
  if (unit == "Hz") return "frequency";
  if (unit == "kWh") return "energy";
  if (unit == "°C") return "temperature";
  return "";
}

static String guessStateClass(const String &unit) {
  if (unit == "kWh") return "total_increasing";
  if (unit == "V" || unit == "A" || unit == "W" || unit == "VA" ||
      unit == "var" || unit == "Hz" || unit == "%" || unit == "°C") return "measurement";
  return "";
}

void mqttResetDiscovery() { s_discoverySent = false; }

static void sendDiscoveryIfNeeded() {
  if (s_discoverySent || !g_config.mqtt_ha_discovery) return;
  if (!s_mqtt.connected()) return;

  String devId = deviceObjectId();

  for (auto &r : g_config.registers) {
    String objectId = devId + "_" + r.name;
    String stateTopic = g_config.mqtt_topic_prefix + "/" + r.name;
    String displayName = r.name_ru.length() ? r.name_ru : r.name;
    bool isEnum = !r.enum_map.empty();
    bool isBitmask = !r.bit_map.empty();
    bool isSwitch = r.writable && r.is_switch && !isEnum && !isBitmask;
    bool isNumber = r.writable && !r.is_switch && !isEnum && !isBitmask; // writable-регистры -- как number-слайдер в HA
    bool isSelect = r.writable && isEnum && !isBitmask; // writable + enum_map -- выпадающий список в HA

    String platform = isSwitch ? "switch" : (isNumber ? "number" : (isSelect ? "select" : "sensor"));
    String cfgTopic = "homeassistant/" + platform + "/" + objectId + "/config";

    String payload = "{";
    payload += "\"name\":\"" + displayName + "\",";
    payload += "\"has_entity_name\":true,";
    payload += "\"state_topic\":\"" + stateTopic + "\",";

    if (isSwitch) {
      // switch-сущность: состояние и команды -- простые "1"/"0", без
      // текстовых ON/OFF, чтобы не городить отдельную схему сравнения --
      // publish уже отдаёт "1"/"0" (см. mqttPublishValue), a onMqttMessage()
      // и так парсит payload через toFloat(), там ничего менять не пришлось.
      String commandTopic = stateTopic + "/set";
      payload += "\"command_topic\":\"" + commandTopic + "\",";
      payload += "\"payload_on\":\"1\",\"payload_off\":\"0\",";
      payload += "\"state_on\":\"1\",\"state_off\":\"0\",";
    } else if (isNumber) {
      // number-сущность: слайдер в HA, живёт вместе с command_topic --
      // изменение значения в HA публикует новое значение туда, мы на
      // него подписаны (см. tryConnect()/onMqttMessage()) и реально
      // пишем в инвертор по Modbus.
      String commandTopic = stateTopic + "/set";
      payload += "\"command_topic\":\"" + commandTopic + "\",";
      payload += "\"min\":" + String(r.min_value, 2) + ",";
      payload += "\"max\":" + String(r.max_value, 2) + ",";
      payload += "\"step\":" + String(r.step, 3) + ",";
      if (r.unit.length()) payload += "\"unit_of_measurement\":\"" + r.unit + "\",";
    } else if (isSelect) {
      // select-сущность: выпадающий список в HA. HA присылает в
      // command_topic ТЕКСТ выбранного варианта (не код) -- обратный
      // поиск текст->код делается в onMqttMessage() по той же enum_map.
      String commandTopic = stateTopic + "/set";
      payload += "\"command_topic\":\"" + commandTopic + "\",";
      String options = "[";
      for (size_t i = 0; i < r.enum_map.size(); i++) {
        if (i) options += ",";
        options += "\"" + r.enum_map[i].second + "\"";
      }
      options += "]";
      payload += "\"options\":" + options + ",";
    } else if (isEnum) {
      // enum-сенсоры HA (только чтение): без unit_of_measurement/state_class,
      // зато со списком допустимых состояний в "options"
      String options = "[";
      for (size_t i = 0; i < r.enum_map.size(); i++) {
        if (i) options += ",";
        options += "\"" + r.enum_map[i].second + "\"";
      }
      options += "]";
      payload += "\"options\":" + options + ",";
      payload += "\"device_class\":\"enum\",";
    } else if (isBitmask) {
      // битовая маска -- произвольная комбинация текста, не fixed-set,
      // поэтому просто текстовый сенсор без unit/device_class/options
    } else {
      String devClass = (r.name == "AmbientHumidity") ? "humidity" : guessDeviceClass(r.unit);
      String stateClass = guessStateClass(r.unit);
      if (r.unit.length()) payload += "\"unit_of_measurement\":\"" + r.unit + "\",";
      if (devClass.length()) payload += "\"device_class\":\"" + devClass + "\",";
      if (stateClass.length()) payload += "\"state_class\":\"" + stateClass + "\",";
    }

    payload += "\"unique_id\":\"" + objectId + "\",";
    payload += "\"device\":{\"identifiers\":[\"" + devId + "\"],"
               "\"name\":\"MUST PV18-3024 VPM II\",\"manufacturer\":\"MUST\","
               "\"model\":\"PV18-3024 VPM II\","
               "\"sw_version\":\"" FIRMWARE_VERSION "\"}";
    payload += "}";

    bool sent = s_mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
    if (!sent) {
      logPrintf("[mqtt] НЕ ОТПРАВЛЕН discovery для '%s' -- пейлоад %u байт, возможно всё ещё превышает буфер MQTT",
                r.name.c_str(), (unsigned)payload.length());
    }
  }
  s_discoverySent = true;
  logPrintf("[mqtt] HA discovery sent for %u registers", (unsigned)g_config.registers.size());
}

void mqttPublishValue(const String &name, float value, const String &unit) {
  // Просто и напрямую: связи нет -- значение не публикуется, и точка.
  // Локальная история (для просмотра в вебе) ведётся отдельно, в
  // history.cpp, независимо от состояния MQTT -- см. main.cpp, там
  // вызывается и mqttPublishValue(), и historyPush() на каждое чтение.
  if (!s_mqtt.connected()) return;
  String topic = g_config.mqtt_topic_prefix + "/" + name;

  const RegisterDef *r = findRegister(name);
  if (r && !r->enum_map.empty()) {
    String text = lookupEnumText(*r, value);
    s_mqtt.publish(topic.c_str(), text.c_str());
    return;
  }
  if (r && !r->bit_map.empty()) {
    String text = lookupBitmaskText(*r, value);
    s_mqtt.publish(topic.c_str(), text.c_str());
    return;
  }
  if (r && r->writable && r->is_switch) {
    s_mqtt.publish(topic.c_str(), String((int)lroundf(value)).c_str());
    return;
  }

  char buf[32];
  dtostrf(value, 0, 3, buf);
  s_mqtt.publish(topic.c_str(), buf);
}

// Обработчик входящих MQTT-сообщений -- сейчас нужен только для
// command_topic слайдеров (number-сущностей). HA публикует новое
// значение в "<prefix>/<name>/set", мы находим регистр по имени темы,
// переводим в сырое значение по scale и реально пишем в инвертор.
static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String topicStr(topic);
  String prefix = g_config.mqtt_topic_prefix + "/";
  if (!topicStr.startsWith(prefix) || !topicStr.endsWith("/set")) return;

  String name = topicStr.substring(prefix.length(), topicStr.length() - 4); // без "/set"

  String payloadStr;
  payloadStr.reserve(length);
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];

  const RegisterDef *r = findRegister(name);
  if (!r || !r->writable) {
    logPrintf("[mqtt] команда на '%s' проигнорирована -- регистр не найден или не writable", name.c_str());
    return;
  }

  uint16_t rawValue;
  float loggedValue;
  bool isSelect = !r->enum_map.empty();

  if (isSelect) {
    // select: HA присылает ТЕКСТ выбранного варианта, а не число --
    // ищем обратное соответствие текст->код в той же enum_map, что
    // используется для чтения (lookupEnumText).
    int code = -1;
    for (auto &kv : r->enum_map) {
      if (kv.second == payloadStr) { code = kv.first; break; }
    }
    if (code < 0) {
      logPrintf("[mqtt] команда на '%s': вариант '%s' не найден в enum_map", name.c_str(), payloadStr.c_str());
      return;
    }
    rawValue = (uint16_t)code;
    loggedValue = (float)code;
  } else {
    float value = payloadStr.toFloat();
    long rawSigned = lroundf(value / r->scale);
    rawValue = (uint16_t)(int16_t)rawSigned;
    loggedValue = value;
  }

  logPrintf("[mqtt] команда из HA: '%s' = %.3f (raw=%u), пишу...", name.c_str(), loggedValue, rawValue);
  ModbusWriteResult result = modbusWriteRegisterSync(r->address, rawValue);

  if (result.success) {
    // сразу отражаем новое значение в state_topic -- не ждём следующего
    // цикла опроса, HA увидит подтверждённое значение немедленно
    String stateTopic = g_config.mqtt_topic_prefix + "/" + name;
    if (isSelect) {
      s_mqtt.publish(stateTopic.c_str(), payloadStr.c_str()); // подтверждённый текст варианта как есть
    } else if (r->is_switch) {
      s_mqtt.publish(stateTopic.c_str(), String((int)lroundf(loggedValue)).c_str());
    } else {
      char buf[32];
      dtostrf(loggedValue, 0, 3, buf);
      s_mqtt.publish(stateTopic.c_str(), buf);
    }
    logPrintf("[mqtt] команда '%s' выполнена успешно", name.c_str());
  } else {
    logPrintf("[mqtt] команда '%s' НЕ выполнена (код 0x%02X) -- значение в HA откатится на реальное при следующем опросе",
              name.c_str(), result.errorCode);
  }
}

static bool tryConnect() {
  if (g_config.mqtt_host.length() == 0) return false;

  s_mqtt.setServer(g_config.mqtt_host.c_str(), g_config.mqtt_port);
  s_mqtt.setBufferSize(2048); // 512 было мало -- discovery-пейлоады с русскими
                              // enum-списками (кириллица вдвое тяжелее в UTF-8)
                              // превышали буфер, publish() молча проваливался

  bool ok;
  if (g_config.mqtt_user.length()) {
    ok = s_mqtt.connect(g_config.mqtt_client_id.c_str(),
                         g_config.mqtt_user.c_str(),
                         g_config.mqtt_password.c_str());
  } else {
    ok = s_mqtt.connect(g_config.mqtt_client_id.c_str());
  }

  if (ok) {
    logPrintln("[mqtt] connected");
    s_discoverySent = false; // отправим discovery заново после (пере)подключения

    unsigned subscribed = 0;
    for (auto &r : g_config.registers) {
      if (!r.writable) continue;
      String cmdTopic = g_config.mqtt_topic_prefix + "/" + r.name + "/set";
      if (s_mqtt.subscribe(cmdTopic.c_str())) subscribed++;
    }
    logPrintf("[mqtt] подписка на команды: %u регистров", subscribed);
  } else {
    logPrintf("[mqtt] connect failed, rc=%d", s_mqtt.state());
  }
  return ok;
}

void mqttSetup() {
  if (g_config.mqtt_tls) {
    // ВНИМАНИЕ: setInsecure() -- канал шифруется (пассивный перехват трафика
    // по пути через интернет не сработает), но сертификат брокера НЕ
    // проверяется (нет защиты от активной MITM-подмены). Для дачно-городского
    // сценария через белый IP это ощутимо лучше, чем открытый MQTT, но не
    // полноценный TLS. Если нужна полная проверка -- добавьте свой CA
    // сертификат через s_secureClient.setCACert(...) вместо setInsecure().
    s_secureClient.setInsecure();
    s_mqtt.setClient(s_secureClient);
    logPrintln("[mqtt] используется TLS (без проверки сертификата брокера)");
  } else {
    s_mqtt.setClient(s_plainClient);
    logPrintln("[mqtt] используется обычное (не шифрованное) соединение");
  }
  s_clientBound = true;
  s_mqtt.setCallback(onMqttMessage);
}

void mqttLoop() {
  if (!s_clientBound) return; // mqttSetup() ещё не вызывался -- не должно происходить в норме
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_config.mqtt_host.length() == 0) return;

  if (!s_mqtt.connected()) {
    unsigned long now = millis();
    if (now - s_lastReconnectAttempt > 5000) {
      s_lastReconnectAttempt = now;
      tryConnect();
    }
    return;
  }

  s_mqtt.loop();
  sendDiscoveryIfNeeded();
}

bool mqttIsConnected() { return s_mqtt.connected(); }
