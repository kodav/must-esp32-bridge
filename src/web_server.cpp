#include "web_server.h"
#include "web_ui.h"
#include "config.h"
#include "log_buffer.h"
#include "mqtt_handler.h"
#include "status_led.h"
#include "history.h"
#include "history_flash.h"
#include "modbus_poll.h"
#include "version.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>

WebServer g_webServer(80);

// HTTP Basic Auth на весь веб-интерфейс и API (кроме /update -- у него
// свой отдельный ota_password через ElegantOTA). Пусто по умолчанию --
// защита выключена, чтобы не оказаться запертым при первой настройке.
// Возвращает true, если запрос можно обрабатывать дальше; если false --
// уже отправлен 401 с запросом Basic Auth, обработчик должен сразу выйти.
static bool checkAuth() {
  if (g_config.web_password.length() == 0) return true; // защита выключена
  if (g_webServer.authenticate(g_config.web_username.c_str(), g_config.web_password.c_str())) {
    return true;
  }
  g_webServer.requestAuthentication();
  return false;
}

static void handleIndex() {
  if (!checkAuth()) return;
  g_webServer.send_P(200, "text/html", INDEX_HTML);
}

static void handleGetConfig() {
  if (!checkAuth()) return;
  g_webServer.send(200, "application/json", configToJson());
}

static void handlePostConfig() {
  if (!checkAuth()) return;
  String body = g_webServer.arg("plain"); // WebServer сам кладёт сырое тело POST сюда

  // Задачи Modbus и MQTT (отдельные ядра) читают g_config в фоне --
  // приостанавливаем обе на время замены конфига целиком, чтобы не
  // словить гонку данных при переприсваивании g_config = c внутри
  // configFromJson().
  modbusTaskPause();
  mqttTaskPause();

  String err;
  if (!configFromJson(body, err)) {
    modbusTaskResume(); // конфиг не менялся -- можно продолжать как ни в чём не бывало
    mqttTaskResume();
    g_webServer.send(400, "text/plain", err);
    return;
  }
  if (!configSave()) {
    modbusTaskResume();
    mqttTaskResume();
    g_webServer.send(500, "text/plain", "failed to save config to flash");
    return;
  }
  mqttResetDiscovery();
  historyReconfigure();
  historyFlashReconfigure();
  g_webServer.send(200, "text/plain", "OK");

  logPrintln("[web] config updated, rebooting in 1.5s...");
  delay(1500); // успеваем реально отправить ответ клиенту перед перезагрузкой
  ESP.restart(); // задача так и останется приостановленной -- неважно, всё равно перезагрузка
}

static void handleGetLogs() {
  if (!checkAuth()) return;
  uint32_t since = 0;
  if (g_webServer.hasArg("since")) {
    since = g_webServer.arg("since").toInt();
  }
  g_webServer.send(200, "application/json", logGetJson(since));
}

static void handleGetVersion() {
  if (!checkAuth()) return;
  String json = "{\"version\":\"" FIRMWARE_VERSION "\",\"build\":\"" FIRMWARE_BUILD_DATE "\"}";
  g_webServer.send(200, "application/json", json);
}

// Статус моста и последние значения — не зависит от успешности MQTT.
// Даже если инвертор не отвечает, веб отдаёт этот JSON (modbus_ok=false).
static void handleGetStatus() {
  if (!checkAuth()) return;
  JsonDocument doc;
  doc["modbus_ok"] = modbusIsHealthy();
  doc["modbus_state"] = modbusStateCode();
  doc["mqtt_ok"] = mqttIsConnected();
  doc["mqtt_state"] = mqttStateCode();
  doc["mqtt_fail_count"] = mqttFailCount();
  doc["wifi_ok"] = (WiFi.status() == WL_CONNECTED);
  doc["auto_mode"]["enabled"] = g_config.auto_mode_enabled;
  doc["auto_mode"]["pv_high"] = g_config.auto_mode_pv_high;
  doc["auto_mode"]["pv_low"] = g_config.auto_mode_pv_low;
  JsonObject vals = doc["values"].to<JsonObject>();
  for (auto &r : g_config.registers) {
    float v;
    if (historyGetLast(r.name, v)) {
      vals[r.name] = v;
    }
  }
  String out;
  serializeJson(doc, out);
  g_webServer.send(200, "application/json", out);
}

static void handlePostModbusWrite() {
  if (!checkAuth()) return;
  String body = g_webServer.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    g_webServer.send(400, "text/plain", "invalid JSON");
    return;
  }
  String name = (const char*)(doc["name"] | "");
  if (!doc["value"].is<float>() && !doc["value"].is<int>()) {
    g_webServer.send(400, "text/plain", "missing 'value'");
    return;
  }
  float value = doc["value"];

  const RegisterDef *target = nullptr;
  for (auto &r : g_config.registers) {
    if (r.name == name) { target = &r; break; }
  }
  if (!target) {
    g_webServer.send(404, "text/plain", "register not found in current list: " + name);
    return;
  }
  if (target->count != 1) {
    g_webServer.send(400, "text/plain", "запись 32-битных регистров (count=2) не поддерживается");
    return;
  }
  if (!target->writable) {
    g_webServer.send(400, "text/plain",
      "'" + name + "' помечен как только для чтения (writable=false в списке регистров) -- "
      "запись в него не предлагается намеренно, это защита от случайной записи в сенсорный "
      "регистр; если уверены, что он реально настраиваемый -- поставьте writable:true в JSON "
      "регистров через веб-конфиг");
    return;
  }

  long rawSigned = lroundf(value / target->scale);
  uint16_t rawValue = (uint16_t)(int16_t)rawSigned; // корректно заворачивает отрицательные значения в two's complement

  logPrintf("[web] запрошена запись '%s' (addr=%u) = %.3f (raw=%u), жду результат...",
            name.c_str(), target->address, value, rawValue);

  ModbusWriteResult result = modbusWriteRegisterSync(target->address, rawValue);

  if (result.success) {
    historyPush(name, value); // сразу обновить кэш для /api/status (не ждать следующего опроса)
    g_webServer.send(200, "text/plain", "OK -- инвертор подтвердил запись");
  } else if (result.errorCode == 0xFF) {
    g_webServer.send(504, "text/plain", "не дождались ответа от инвертора за 3с (проверьте связь по RS485)");
  } else {
    g_webServer.send(502, "text/plain", "инвертор вернул ошибку Modbus, код 0x" + String(result.errorCode, HEX));
  }
}

static void handleGetHistoryExportCsv() {
  if (!checkAuth()) return;
  g_webServer.sendHeader("Content-Disposition", "attachment; filename=\"must_pv18_history.csv\"");
  g_webServer.setContentLength(CONTENT_LENGTH_UNKNOWN); // потоковая (chunked) отдача -- размер заранее неизвестен
  g_webServer.send(200, "text/csv", "");
  g_webServer.sendContent("timestamp,register,value\n");
  historyExportCsv([](const String &chunk) {
    g_webServer.sendContent(chunk);
  });
}

static void handleGetFlashHistoryExportCsv() {
  if (!checkAuth()) return;
  g_webServer.sendHeader("Content-Disposition", "attachment; filename=\"must_pv18_history_flash.csv\"");
  g_webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_webServer.send(200, "text/csv", "");
  g_webServer.sendContent("timestamp,register,value\n");
  historyFlashExportCsv([](const String &chunk) {
    g_webServer.sendContent(chunk);
  });
}

static void handleGetHistory() {
  if (!checkAuth()) return;
  if (!g_webServer.hasArg("register")) {
    g_webServer.send(400, "text/plain", "missing 'register' query param");
    return;
  }
  String reg = g_webServer.arg("register");
  size_t points = 300;
  if (g_webServer.hasArg("points")) {
    points = g_webServer.arg("points").toInt();
    if (points < 1) points = 1;
    if (points > 2000) points = 2000; // защита от случайного гигантского запроса
  }
  g_webServer.send(200, "application/json", historyGetJson(reg, points));
}

void webServerSetup() {
  g_webServer.on("/", HTTP_GET, handleIndex);
  g_webServer.on("/api/config", HTTP_GET, handleGetConfig);
  g_webServer.on("/api/config", HTTP_POST, handlePostConfig);
  g_webServer.on("/api/logs", HTTP_GET, handleGetLogs);
  g_webServer.on("/api/history", HTTP_GET, handleGetHistory);
  g_webServer.on("/api/history/export.csv", HTTP_GET, handleGetHistoryExportCsv);
  g_webServer.on("/api/history/export_flash.csv", HTTP_GET, handleGetFlashHistoryExportCsv);
  g_webServer.on("/api/version", HTTP_GET, handleGetVersion);
  g_webServer.on("/api/status", HTTP_GET, handleGetStatus);
  g_webServer.on("/api/modbus/write", HTTP_POST, handlePostModbusWrite);

  // logInit() уже вызван в main.cpp::setup() до logInstallEspLogCapture() --
  // повторный вызов здесь стёр бы уже накопленные ранние логи загрузки.

  ElegantOTA.begin(&g_webServer,
                    g_config.ota_password.length() ? "admin" : nullptr,
                    g_config.ota_password.length() ? g_config.ota_password.c_str() : nullptr);
  // ПРИМЕЧАНИЕ: сигнатура onStart/onEnd приведена по типовому API ElegantOTA
  // (onStart() без параметров, onEnd(bool success)) -- если у установленной
  // версии библиотеки сигнатура отличается, компилятор укажет точную строку
  // с ошибкой, поправьте под неё (сама индикация OTA -- не критичная часть
  // проекта, при необходимости можно временно закомментировать этот блок).
  ElegantOTA.onStart([]() {
    logPrintln("[ota] обновление началось");
    statusLedSetOtaActive(true);
  });
  ElegantOTA.onEnd([](bool success) {
    logPrintf("[ota] обновление завершено, успех=%d", (int)success);
    statusLedSetOtaActive(false);
  });

  g_webServer.begin();
  logPrintln("[web] server started on port 80");
}

void webServerLoop() {
  // Веб всегда в основном loop() (ядро 1). Modbus и MQTT — отдельные
  // FreeRTOS-задачи на ядре 0: зависания RS485/MQTT не блокируют HTTP.
  g_webServer.handleClient();
}