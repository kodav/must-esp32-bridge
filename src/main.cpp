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
#include "history_flash.h"
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
  mqttTaskStart();
  historySetup();
  historyFlashSetup();
  tempHumiditySetup();

  logPrintln("[boot] инициализация завершена");
}

// EnergyUseMode: 1=SBU, 2=SUB, 3=UTI, 4=SOL (addr 20109)
static const uint16_t kModeSBU = 1;
static const uint16_t kModeUTI = 3;
static const uint16_t kEnergyUseModeAddr = 20109;

static int s_lastAutoMode = -1;          // последний режим, который мы сами записали (-1 = ещё не писали)
static uint32_t s_lastAutoSwitchMs = 0;  // millis() момента последней записи

// Авто-переключение SBU <-> UTI по PvVoltage с гистерезисом и min-interval.
// Неблокирующая запись — веб не зависает.
static void autoModeOnPvVoltage(float pvV) {
  if (!g_config.auto_mode_enabled) return;

  float high = g_config.auto_mode_pv_high;
  float low = g_config.auto_mode_pv_low;
  if (low >= high) return; // защита на случай кривого конфига в RAM

  int desired = -1;
  if (pvV >= high) {
    desired = (int)kModeSBU; // день / есть генерация
  } else if (pvV <= low) {
    desired = (int)kModeUTI; // ночь / почти нет PV
  } else {
    return; // зона гистерезиса — не трогаем
  }

  // Уже в нужном режиме (по нашей последней команде) — не пишем снова
  if (s_lastAutoMode == desired) return;

  uint32_t now = millis();
  uint32_t minMs = g_config.auto_mode_min_interval_s * 1000UL;
  if (s_lastAutoMode >= 0 && (now - s_lastAutoSwitchMs) < minMs) {
    return; // слишком рано после прошлого переключения
  }

  uint16_t addr = kEnergyUseModeAddr;
  for (const auto &r : g_config.registers) {
    if (r.name == "EnergyUseMode") { addr = r.address; break; }
  }

  if (!modbusWriteRegisterAsync(addr, (uint16_t)desired)) {
    logPrintln("[auto_mode] очередь записи переполнена, пропуск");
    return;
  }

  const char *modeName = (desired == (int)kModeSBU) ? "SBU" : "UTI";
  logPrintf("[auto_mode] PvVoltage=%.1fV → EnergyUseMode=%s (%d) (high=%.1f low=%.1f)",
            pvV, modeName, desired, high, low);
  s_lastAutoMode = desired;
  s_lastAutoSwitchMs = now;
  historyPush("EnergyUseMode", (float)desired);
}

// Уберите лямбду, используйте обычную функцию
static void processModbusData(const String &name, float value, const String &unit) {
    // Минимизируем создание объектов
    char msg[128];
    snprintf(msg, sizeof(msg), "[data] %s = %.3f %s", name.c_str(), value, unit.c_str());
    logPrintln(String(msg));
    
    mqttPublishValue(name, value, unit);
    historyPush(name, value);
    historyFlashPush(name, value);

    if (name == "PvVoltage") {
      autoModeOnPvVoltage(value);
    } else if (name == "EnergyUseMode") {
      // Синхронизируем «последний известный режим» с фактическим опросом,
      // чтобы авто-логика не писала лишний раз после ручного переключения.
      int m = (int)lroundf(value);
      if (m == (int)kModeSBU || m == (int)kModeUTI) {
        s_lastAutoMode = m;
      }
    }
}

void loop() {
    webServerLoop();
    ElegantOTA.loop();
    statusLedLoop();

    modbusPollLoop(processModbusData);  // Передаем функцию, не лямбду

    historyFlashLoop();
    tempHumidityLoop();
}

