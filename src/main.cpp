#include <Arduino.h>
#include <WiFi.h>
#include <ElegantOTA.h>

#include "config.h"
#include "log_buffer.h"
#include "web_server.h"
#include "modbus_transport.h"
#include "modbus_poll.h"
#include "mqtt_handler.h"
#include "status_led.h"
#include "history.h"
#include "version.h"
#include "temp_humidity.h"

static const char *AP_SSID = "MUST-PV18-Setup";
static const char *AP_PASSWORD = "12345678"; // сменить, если критично для вашей сети
static const char *DEVICE_HOSTNAME = "must-pv18-esp32"; // имя в DHCP/mDNS вместо esp32s3-XXXXXX

static void connectWifiOrStartAP() {
  if (g_config.wifi_ssid.length() == 0) {
    Serial.println("[wifi] SSID не задан, поднимаю точку доступа для настройки...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("[wifi] AP '%s' запущена, IP: %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());
    statusLedSetApMode(true);
    return;
  }

  // ВАЖНО: setHostname() должен вызываться ДО WiFi.mode() -- официальная
  // документация Arduino-ESP32 это прямо требует, иначе имя не применится
  // и останется дефолтное espressif/esp32s3-XXXXXX. Порядок именно такой.
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(g_config.wifi_ssid.c_str(), g_config.wifi_password.c_str());
  Serial.printf("[wifi] подключение к '%s'...\n", g_config.wifi_ssid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] подключено, IP: %s\n", WiFi.localIP().toString().c_str());
    statusLedSetApMode(false);
    // NTP -- нужен для настоящих таймстампов в локальной истории (history.cpp).
    // Best-effort: если не засинкается, historyPush() просто будет писать
    // время около 1970 года, фронтенд это распознает и предупредит.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("[wifi] не удалось подключиться, поднимаю точку доступа для настройки...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("[wifi] AP '%s' запущена, IP: %s\n", AP_SSID,
                  WiFi.softAPIP().toString().c_str());
    statusLedSetApMode(true);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  logInit();                 // сначала чистый буфер...
  logInstallEspLogCapture(); // ...потом ловим системные логи с самого старта
  Serial.println("\n[boot] MUST PV18 <-> MQTT bridge starting");
  logPrintf("[boot] версия прошивки: %s (собрано %s)", FIRMWARE_VERSION, FIRMWARE_BUILD_DATE);

  configLoad();
  statusLedSetup();
  connectWifiOrStartAP();

  webServerSetup();     // включает и логи (SSE), и ElegantOTA, и конфиг-API
  modbusTaskStart();
  mqttSetup();
  historySetup();
  tempHumiditySetup();

  logPrintln("[boot] инициализация завершена");
}

void loop() {
  webServerLoop();       // синхронный WebServer -- нужно опрашивать вручную
  ElegantOTA.loop();
  statusLedLoop();

  modbusPollLoop([](const String &name, float value, const String &unit) {
    logPrintf("[data] %s = %.3f %s", name.c_str(), value, unit.c_str());
    mqttPublishValue(name, value, unit);
    historyPush(name, value);
  });

  mqttLoop();
  tempHumidityLoop();
}
