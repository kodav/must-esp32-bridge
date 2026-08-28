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
//
// ВАЖНО (исправление зависания):
// - НЕ вызываем WiFi.disconnect/reconnect из этой задачи -- это гонка
//   с веб-сервером/ElegantOTA на ядре 1 и приводит к полному зависанию
//   сетевого стека (нет даже ping).
// - Перед каждым connect() принудительно stop() клиента + короткий
//   socket timeout, чтобы lwIP не уходил в длинные SYN-ретраи.
// - При длительном отсутствии связи делаем только паузу в задаче,
//   WiFi-стек восстанавливается сам.
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

// Состояние для веб/логов (читается из других задач).
// 0=ok, 1=no_host, 2=no_wifi, 3=connecting, 4=fail
static volatile int s_stateCode = 1;
static volatile int s_failCount = 0;
static volatile int s_lastRc = 0;

// Короткий таймаут сокета: достаточно для нормальной сети, не даёт
// lwIP уходить в минутные ретраи при недоступном брокере.
static const unsigned long MQTT_SOCKET_TIMEOUT_MS = 3000;
// Keepalive MQTT (сек) — шлётся в CONNECT; брокер/клиент ждут ~1–1.5×.
static const uint16_t MQTT_KEEPALIVE_SEC = 15;
// Двусторонний probe: публикуем в topic и ждём свой echo.
// При blackhole на роутере publish() «успешен» (буфер TCP), но echo
// не приходит — только так надёжно ловим «зелёный MQTT при мёртвом брокере».
static const unsigned long MQTT_PROBE_INTERVAL_MS = 15000;  // шлём ping каждые 15 с
static const unsigned long MQTT_PROBE_DEAD_MS     = 35000;  // нет echo → обрыв

static volatile unsigned long s_lastProbeRxMs = 0;   // когда получили echo / connect
static unsigned long s_lastProbeTxMs = 0;            // когда отправили probe
static bool s_probePending = false;

static void setMqttState(int code, const char *logMsg = nullptr) {
  if (s_stateCode != code && logMsg) {
    logPrintf("[mqtt] состояние: %s", logMsg);
  }
  s_stateCode = code;
}

static String probeTopic() {
  return g_config.mqtt_topic_prefix + "/__ping";
}

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
// Возвращает false, если publish не удался (сокет уже закрыт).
// ВАЖНО: успешный publish НЕ доказывает, что брокер жив (blackhole TCP).
static bool publishOne(const String &name, float value) {
  if (!s_mqtt.connected()) return false;
  String topic = g_config.mqtt_topic_prefix + "/" + name;
  bool ok = false;

  const RegisterDef *r = findRegister(name);
  if (r && !r->enum_map.empty()) {
    String text = lookupEnumText(*r, value);
    ok = s_mqtt.publish(topic.c_str(), text.c_str());
  } else if (r && !r->bit_map.empty()) {
    String text = lookupBitmaskText(*r, value);
    ok = s_mqtt.publish(topic.c_str(), text.c_str());
  } else if (r && r->writable && r->is_switch) {
    ok = s_mqtt.publish(topic.c_str(), String((int)lroundf(value)).c_str());
  } else {
    char buf[32];
    dtostrf(value, 0, 3, buf);
    ok = s_mqtt.publish(topic.c_str(), buf);
  }

  if (!ok) {
    logPrintf("[mqtt] проблема: publish '%s' не удался -- связь с брокером потеряна?", name.c_str());
  }
  return ok;
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

// true = очередь обработана без ошибок publish; false = обрыв, нужна переподписка
static bool drainPublishQueue() {
  if (!s_mqtt.connected()) return true;
  PublishRequest req;
  while (xQueueReceive(s_publishQueue, &req, 0) == pdTRUE) {
    if (!publishOne(String(req.name), req.value)) {
      // Очищаем остаток очереди — значения всё равно устарели
      while (xQueueReceive(s_publishQueue, &req, 0) == pdTRUE) {}
      return false;
    }
  }
  return true;
}

// Обработчик входящих MQTT-сообщений -- выполняется внутри MQTT-задачи
// (вызывается синхронно из s_mqtt.loop()), поэтому блокирующий вызов
// modbusWriteRegisterSync() здесь безопасен -- не трогает веб-сервер.
static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String topicStr(topic);

  // Echo нашего probe — единственное надёжное подтверждение, что брокер
  // реально принимает и отдаёт пакеты (а не «полуоткрытый» TCP).
  if (topicStr == probeTopic()) {
    s_lastProbeRxMs = millis();
    s_probePending = false;
    return;
  }

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

// Принудительно закрыть сокет и сбросить состояние PubSubClient.
// Без этого после обрыва connect() может надолго зависнуть в lwIP.
static void hardStopClient() {
  s_mqtt.disconnect();
  if (g_config.mqtt_tls) {
    s_secureClient.stop();
  } else {
    s_plainClient.stop();
  }
}

static bool tryConnect() {
  if (g_config.mqtt_host.length() == 0) return false;

  // Всегда начинаем с чистого сокета -- иначе после обрыва lwIP
  // может держать полуоткрытое соединение и блокироваться.
  hardStopClient();

  s_mqtt.setServer(g_config.mqtt_host.c_str(), g_config.mqtt_port);
  s_mqtt.setBufferSize(2048);
  s_mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
  s_mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_MS / 1000); // в секундах у PubSubClient

  // Короткий socket timeout ДО connect -- ограничивает время блокировки
  // в TCP SYN / TLS handshake. setTimeout влияет на read/write и
  // connect в Arduino-ESP32 WiFiClient.
  if (g_config.mqtt_tls) {
    s_secureClient.setTimeout(MQTT_SOCKET_TIMEOUT_MS);
  } else {
    s_plainClient.setTimeout(MQTT_SOCKET_TIMEOUT_MS);
  }

  unsigned long t0 = millis();
  bool ok;
  if (g_config.mqtt_user.length()) {
    ok = s_mqtt.connect(g_config.mqtt_client_id.c_str(),
                         g_config.mqtt_user.c_str(),
                         g_config.mqtt_password.c_str());
  } else {
    ok = s_mqtt.connect(g_config.mqtt_client_id.c_str());
  }
  unsigned long took = millis() - t0;

  if (ok) {
    logPrintf("[mqtt] connected (%lu ms)", took);
    s_discoverySent = false;
    s_lastRc = 0;
    s_lastProbeRxMs = millis(); // считаем connect успешной «жизнью»
    s_lastProbeTxMs = 0;
    s_probePending = false;

    // Подписка на probe-топик (свой echo) + команды writable-регистров
    String ping = probeTopic();
    if (!s_mqtt.subscribe(ping.c_str())) {
      logPrintf("[mqtt] не удалось подписаться на probe '%s'", ping.c_str());
    }

    unsigned subscribed = 0;
    for (auto &r : g_config.registers) {
      if (!r.writable) continue;
      String cmdTopic = g_config.mqtt_topic_prefix + "/" + r.name + "/set";
      if (s_mqtt.subscribe(cmdTopic.c_str())) subscribed++;
    }
    logPrintf("[mqtt] подписка на команды: %u регистров + probe", subscribed);
  } else {
    s_lastRc = s_mqtt.state();
    // rc: -4 timeout, -3 lost, -2 failed, -1 disconnected, 1-5 protocol
    logPrintf("[mqtt] connect failed, rc=%d, took %lu ms (проблема связи с брокером)",
              s_lastRc, took);
    hardStopClient();
  }

  return ok;
}

static void mqttTaskFn(void *) {
  logPrintln("[mqtt] задача запущена (ядро 0)");

  if (g_config.mqtt_tls) {
    s_secureClient.setInsecure();
    s_secureClient.setTimeout(MQTT_SOCKET_TIMEOUT_MS);
    s_mqtt.setClient(s_secureClient);
    logPrintln("[mqtt] используется TLS (без проверки сертификата брокера)");
  } else {
    s_plainClient.setTimeout(MQTT_SOCKET_TIMEOUT_MS);
    s_mqtt.setClient(s_plainClient);
    logPrintln("[mqtt] используется обычное (не шифрованное) соединение");
  }
  s_mqtt.setCallback(onMqttMessage);
  s_mqtt.setKeepAlive(MQTT_KEEPALIVE_SEC);
  s_mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_MS / 1000);

  int connectFailCount = 0;

  for (;;) {
    if (g_config.mqtt_host.length() == 0) {
      s_connectedFlag = false;
      s_failCount = 0;
      setMqttState(1, "хост не задан -- MQTT выключен");
      if (s_mqtt.connected()) hardStopClient();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      s_connectedFlag = false;
      setMqttState(2, "нет WiFi -- MQTT ждёт сеть");
      if (s_mqtt.connected()) hardStopClient();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (!s_mqtt.connected()) {
      s_connectedFlag = false;
      unsigned long now = millis();
      // Интервал между попытками: базово 5 с, при частых ошибках
      // увеличиваем паузу, чтобы не долбить недоступный брокер.
      unsigned long retryInterval = 5000;
      if (connectFailCount > 5)  retryInterval = 15000;
      if (connectFailCount > 15) retryInterval = 60000;

      if (connectFailCount == 0) {
        setMqttState(3, "подключение к брокеру...");
      } else {
        setMqttState(4, nullptr); // fail -- детали уже в логе connect
      }

      if (now - s_lastReconnectAttempt >= retryInterval) {
        s_lastReconnectAttempt = now;
        setMqttState(3, "подключение к брокеру...");

        if (tryConnect()) {
          connectFailCount = 0;
          s_failCount = 0;
        } else {
          connectFailCount++;
          s_failCount = connectFailCount;
          setMqttState(4, nullptr);
          if (connectFailCount == 1 || connectFailCount == 6 || connectFailCount == 16) {
            logPrintf("[mqtt] проблема: брокер недоступен (попытка %d, следующий интервал %lu с)",
                      connectFailCount, retryInterval / 1000UL);
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    s_connectedFlag = true;
    connectFailCount = 0;
    s_failCount = 0;
    setMqttState(0, "подключен");

    // Ограничение времени выполнения loop() -- не даём MQTT-задаче
    // надолго занимать ядро 0 даже при большом потоке сообщений.
    unsigned long loopStart = millis();
    bool publishOk = true;
    while (s_mqtt.connected() && (millis() - loopStart) < 100) {
      if (!s_mqtt.loop()) {
        break;
      }
      sendDiscoveryIfNeeded();
      if (!drainPublishQueue()) {
        publishOk = false;
        break;
      }
      delay(1);
    }

    // Обрыв, обнаруженный loop()/publish
    if (!s_mqtt.connected() || !publishOk) {
      hardStopClient();
      s_connectedFlag = false;
      setMqttState(4, "связь с брокером оборвалась (WiFi есть, брокер недоступен)");
      logPrintln("[mqtt] проблема: соединение сброшено, будет переподключение");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Доп. проверка нижележащего TCP-сокета
    bool tcpOk = g_config.mqtt_tls ? s_secureClient.connected() : s_plainClient.connected();
    if (!tcpOk) {
      logPrintln("[mqtt] проблема: TCP-сокет закрыт, брокер недоступен");
      hardStopClient();
      s_connectedFlag = false;
      setMqttState(4, "TCP-сокет закрыт");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // --- Двусторонний probe (ловим blackhole на роутере) ---
    unsigned long nowMs = millis();
    if (nowMs - s_lastProbeTxMs >= MQTT_PROBE_INTERVAL_MS) {
      s_lastProbeTxMs = nowMs;
      String ping = probeTopic();
      // payload = millis, чтобы брокер точно отдал «новое» сообщение
      char payload[16];
      snprintf(payload, sizeof(payload), "%lu", nowMs);
      if (s_mqtt.publish(ping.c_str(), payload, false)) {
        s_probePending = true;
      } else {
        logPrintln("[mqtt] проблема: probe publish не удался");
        hardStopClient();
        s_connectedFlag = false;
        setMqttState(4, "probe publish failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
    }

    // Нет echo дольше MQTT_PROBE_DEAD_MS → брокер недоступен
    if (s_lastProbeRxMs != 0 && (nowMs - s_lastProbeRxMs) > MQTT_PROBE_DEAD_MS) {
      logPrintf("[mqtt] проблема: нет ответа брокера %lu с (probe timeout) -- "
                "WiFi есть, но MQTT-путь заблокирован или брокер мёртв",
                (nowMs - s_lastProbeRxMs) / 1000UL);
      hardStopClient();
      s_connectedFlag = false;
      s_probePending = false;
      setMqttState(4, "брокер не отвечает (probe timeout)");
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
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

const char *mqttStateCode() {
  switch (s_stateCode) {
    case 0: return "ok";
    case 1: return "no_host";
    case 2: return "no_wifi";
    case 3: return "connecting";
    default: return "fail";
  }
}

int mqttFailCount() { return s_failCount; }

void mqttTaskPause() {
  if (s_taskHandle) vTaskSuspend(s_taskHandle);
}

void mqttTaskResume() {
  if (s_taskHandle) vTaskResume(s_taskHandle);
}
