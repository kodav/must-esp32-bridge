#include "mqtt_handler.h"
#include "config.h"
#include "log_buffer.h"
#include "version.h"
#include "modbus_poll.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------------------------------------------------------------
// MQTT работает в ОТДЕЛЬНОЙ FreeRTOS-задаче (ядро 0, вместе с Modbus).
//
// Причина: s_mqtt.connect() -- блокирующий вызов на уровне TCP-сокета.
// Если брокер физически недоступен (отключили электричество/интернет на
// стороне брокера, обрыв связи и т.п.), попытка подключения может висеть
// заметное время, и это повторяется каждые 5 секунд, пока связи нет. Раз
// было проверено на практике: при таком сценарии, если это выполняется
// в ОСНОВНОМ loop() (там же, где веб-сервер) -- веб-интерфейс становится
// недоступен на весь период отключения, а не только тормозит. Задача
// полностью изолирует эту проблему от веб-сервера, как и для Modbus.
//
// mqttPublishValue() -- тонкий producer, вызывается из ЛЮБОЙ задачи
// (главным образом из main loop(), когда приходят результаты опроса
// Modbus через свою очередь): просто кладёт запрос в FreeRTOS-очередь,
// саму публикацию (с учётом enum_map/bit_map/is_switch) делает уже
// MQTT-задача -- PubSubClient не потокобезопасен, трогать его можно
// только из задачи, которая им владеет.
// ---------------------------------------------------------------------

struct PublishRequest {
  char name[40];
  float value;
};

static WiFiClient s_plainClient;
static WiFiClientSecure s_secureClient;
static PubSubClient s_mqtt;
static QueueHandle_t s_publishQueue = nullptr;
static TaskHandle_t s_taskHandle = nullptr;

static bool s_discoverySent = false;
static volatile bool s_connectedFlag = false; // читается из других задач (statusLed) -- не трогаем s_mqtt напрямую снаружи
static unsigned long s_lastReconnectAttempt = 0;

// В начале файла, после других глобальных переменных добавьте:
static int s_connectFailCount = 0;

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
      String commandTopic = stateTopic + "/set";
      payload += "\"command_topic\":\"" + commandTopic + "\",";
      payload += "\"payload_on\":\"1\",\"payload_off\":\"0\",";
      payload += "\"state_on\":\"1\",\"state_off\":\"0\",";
    } else if (isNumber) {
      String commandTopic = stateTopic + "/set";
      payload += "\"command_topic\":\"" + commandTopic + "\",";
      payload += "\"min\":" + String(r.min_value, 2) + ",";
      payload += "\"max\":" + String(r.max_value, 2) + ",";
      payload += "\"step\":" + String(r.step, 3) + ",";
      payload += "\"mode\":\"box\",";
      if (r.unit.length()) payload += "\"unit_of_measurement\":\"" + r.unit + "\",";
    } else if (isSelect) {
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

// Реальная публикация с учётом enum_map/bit_map/is_switch -- вызывается
// ТОЛЬКО из MQTT-задачи (владеет s_mqtt), см. drainPublishQueue().
static void publishOne(const String &name, float value) {
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

void mqttPublishValue(const String &name, float value, const String &unit) {
  if (!s_publishQueue) return;
  PublishRequest req;
  strncpy(req.name, name.c_str(), sizeof(req.name) - 1);
  req.name[sizeof(req.name) - 1] = 0;
  req.value = value;
  // не блокируем вызывающую задачу; если очередь вдруг переполнена
  // (например, долгий обрыв MQTT) -- значение просто теряется, не
  // страшно, следующий цикл опроса принесёт свежее
  xQueueSend(s_publishQueue, &req, 0);
}

static void drainPublishQueue() {
  if (!s_mqtt.connected()) return;
  PublishRequest req;
  while (xQueueReceive(s_publishQueue, &req, 0) == pdTRUE) {
    publishOne(String(req.name), req.value);
  }
}

// Обработчик входящих MQTT-сообщений -- выполняется внутри MQTT-задачи
// (вызывается синхронно из s_mqtt.loop()), поэтому блокирующий вызов
// modbusWriteRegisterSync() здесь безопасен -- не трогает веб-сервер.
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
    String stateTopic = g_config.mqtt_topic_prefix + "/" + name;
    if (isSelect) {
      s_mqtt.publish(stateTopic.c_str(), payloadStr.c_str());
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

static void forceWiFiReset() {
    logPrintln("[mqtt] принудительный сброс WiFi стека");
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.reconnect();
    delay(100);
}

static bool tryConnect() {
    if (g_config.mqtt_host.length() == 0) return false;

    s_mqtt.setServer(g_config.mqtt_host.c_str(), g_config.mqtt_port);
    s_mqtt.setBufferSize(2048);

    // ДОБАВИТЬ: ограничение времени подключения
    unsigned long startAttempt = millis();
    const unsigned long CONNECT_TIMEOUT = 5000; // 5 секунд
    
    bool ok;
    if (g_config.mqtt_user.length()) {
        ok = s_mqtt.connect(g_config.mqtt_client_id.c_str(),
                             g_config.mqtt_user.c_str(),
                             g_config.mqtt_password.c_str());
    } else {
        ok = s_mqtt.connect(g_config.mqtt_client_id.c_str());
    }

    // Проверка, не зависло ли подключение
    if (!ok && (millis() - startAttempt) >= CONNECT_TIMEOUT) {
        logPrintln("[mqtt] timeout при подключении к брокеру");
        s_mqtt.disconnect(); // принудительно закрыть соединение
        return false;
    }

    if (ok) {
        logPrintln("[mqtt] connected");
        s_discoverySent = false;

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

    if (!ok) {
        s_connectFailCount++;
        if (s_connectFailCount > 5) {
            forceWiFiReset();
            s_connectFailCount = 0;
        }
    } else {
        s_connectFailCount = 0;
    }
    
    return ok;
}

static void mqttTaskFn(void *) {
    logPrintln("[mqtt] задача запущена (ядро 0)");

    if (g_config.mqtt_tls) {
        s_secureClient.setInsecure();
        s_secureClient.setTimeout(8000);
        s_mqtt.setClient(s_secureClient);
        logPrintln("[mqtt] используется TLS (без проверки сертификата брокера)");
    } else {
        s_plainClient.setTimeout(8000);
        s_mqtt.setClient(s_plainClient);
        logPrintln("[mqtt] используется обычное (не шифрованное) соединение");
    }
    s_mqtt.setCallback(onMqttMessage);

    int connectFailCount = 0;
    
    for (;;) {
        if (WiFi.status() != WL_CONNECTED || g_config.mqtt_host.length() == 0) {
            s_connectedFlag = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_mqtt.connected()) {
            s_connectedFlag = false;
            unsigned long now = millis();
            if (now - s_lastReconnectAttempt > 5000) {
                s_lastReconnectAttempt = now;
                
                // Экспоненциальная задержка при повторных ошибках
                if (connectFailCount > 10) {
                    logPrintln("[mqtt] слишком много ошибок, пауза 60с");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                    connectFailCount = 0;
                }
                
                if (tryConnect()) {
                    connectFailCount = 0;
                } else {
                    connectFailCount++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        s_connectedFlag = true;
        connectFailCount = 0;
        
        // Ограничение времени выполнения loop()
        unsigned long loopStart = millis();
        while (s_mqtt.connected() && (millis() - loopStart) < 100) {
            s_mqtt.loop();
            sendDiscoveryIfNeeded();
            drainPublishQueue();
            delay(1);
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void mqttTaskStart() {
  s_publishQueue = xQueueCreate(64, sizeof(PublishRequest));

  xTaskCreatePinnedToCore(mqttTaskFn, "mqtt", 6144, nullptr, 1, &s_taskHandle, 0);

  logPrintln("[mqtt] задача создана (ядро 0, изолирована от веб-сервера на ядре 1)");
}

bool mqttIsConnected() { return s_connectedFlag; }

void mqttTaskPause() {
  if (s_taskHandle) vTaskSuspend(s_taskHandle);
}

void mqttTaskResume() {
  if (s_taskHandle) vTaskResume(s_taskHandle);
}
