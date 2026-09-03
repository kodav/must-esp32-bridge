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
#include "coulomb_counter.h"   // добавить к остальным include

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
  coulombCounterBegin();
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

static int s_lastAutoMode = -1;          // последний целевой/известный режим (-1 = ещё не знаем)
static uint32_t s_lastAutoSwitchMs = 0;  // millis() момента последней попытки записи
static bool s_havePvV = false;
static float s_lastPvV = 0.0f;
static bool s_haveEnergyMode = false;
static int s_polledEnergyMode = -1;      // фактический режим с инвертора (-1 = ещё не читали)

// Удержание порога: desired (SBU/UTI) держится непрерывно с s_holdSinceMs.
// Смена зоны или уход в гистерезис сбрасывает таймер — короткий скачок
// 30↔68 В не дотягивает до min_interval_s и не переключает режим.
static int s_holdDesired = -1;
static uint32_t s_holdSinceMs = 0;

// Авто-переключение SBU <-> UTI по PvVoltage.
// min_interval_s = длительность СТАБИЛЬНОГО нахождения за порогом, а не
// пауза между двумя записями. Вызывать только при новом PvVoltage.
static void autoModeOnPvSample(float pvV) {
  if (!g_config.auto_mode_enabled) return;

  float high = g_config.auto_mode_pv_high;
  float low = g_config.auto_mode_pv_low;
  if (low >= high) return;

  uint32_t now = millis();
  int zone = -1; // -1 = гистерезис / неизвестно
  if (pvV >= high) {
    zone = (int)kModeSBU;
  } else if (pvV <= low) {
    zone = (int)kModeUTI;
  }

  // Смена зоны или уход в середину — заново начинаем отсчёт стабильности
  if (zone != s_holdDesired) {
    s_holdDesired = zone;
    s_holdSinceMs = now;
    if (zone < 0) {
      // в гистерезисе ничего не делаем
    }
    return;
  }
  if (zone < 0) return;

  // Уже в нужном режиме на инверторе — считаем цель достигнутой
  if (s_haveEnergyMode && s_polledEnergyMode == zone) {
    s_lastAutoMode = zone;
    return;
  }
  // Недавно сами отправили эту команду — ждём settle, не дублируем
  if (s_lastAutoMode == zone) {
    const uint32_t settleMs = 5000;
    if (s_lastAutoSwitchMs != 0 && (now - s_lastAutoSwitchMs) < settleMs) return;
  }

  uint32_t holdMs = g_config.auto_mode_min_interval_s * 1000UL;
  if ((now - s_holdSinceMs) < holdMs) {
    return; // ещё не продержали порог достаточно долго
  }

  uint16_t addr = kEnergyUseModeAddr;
  for (const auto &r : g_config.registers) {
    if (r.name == "EnergyUseMode") { addr = r.address; break; }
  }

  if (!modbusWriteRegisterAsync(addr, (uint16_t)zone)) {
    logPrintln("[auto_mode] очередь записи переполнена, пропуск");
    return;
  }

  const char *modeName = (zone == (int)kModeSBU) ? "SBU" : "UTI";
  const char *fromName = "?";
  if (s_haveEnergyMode) {
    if (s_polledEnergyMode == (int)kModeSBU) fromName = "SBU";
    else if (s_polledEnergyMode == (int)kModeUTI) fromName = "UTI";
    else if (s_polledEnergyMode == 2) fromName = "SUB";
    else if (s_polledEnergyMode == 4) fromName = "SOL";
  }
  uint32_t heldS = (now - s_holdSinceMs) / 1000UL;
  logPrintf("[auto_mode] PvVoltage=%.1fV стабильно %us → EnergyUseMode %s→%s (%d) (high=%.1f low=%.1f hold=%us)",
            pvV, (unsigned)heldS, fromName, modeName, zone, high, low,
            (unsigned)g_config.auto_mode_min_interval_s);
  s_lastAutoMode = zone;
  s_lastAutoSwitchMs = now;
  // После успешной постановки записи сбрасываем hold, чтобы не слать повтор
  // на каждом следующем опросе, пока инвертор не подтвердит режим.
  s_holdDesired = -1;
  s_holdSinceMs = now;
  historyPush("EnergyUseMode", (float)zone);
}

static void processModbusData(const String &name, float value, const String &unit) {
    coulombCounterFeed(name, value);   // <-- ваттметр/кулонометр питается от потока регистров
    
    char msg[128];
    snprintf(msg, sizeof(msg), "[data] %s = %.3f %s", name.c_str(), value, unit.c_str());
    logPrintln(String(msg));

    mqttPublishValue(name, value, unit);
    historyPush(name, value);
    historyFlashPush(name, value);

    if (name == "PvVoltage") {
      s_lastPvV = value;
      s_havePvV = true;
      autoModeOnPvSample(value);
    } else if (name == "EnergyUseMode") {
      int m = (int)lroundf(value);
      s_polledEnergyMode = m;
      s_haveEnergyMode = true;
      uint32_t now = millis();
      const uint32_t settleMs = 5000;
      bool settled = (s_lastAutoSwitchMs == 0) || (now - s_lastAutoSwitchMs) >= settleMs;
      if (settled && (m == (int)kModeSBU || m == (int)kModeUTI)) {
        s_lastAutoMode = m;
      }
    }
}

void loop() {
    webServerLoop();
    ElegantOTA.loop();
    statusLedLoop();

    modbusPollLoop(processModbusData);  // Передаем функцию, не лямбду
    coulombCounterLoop();      // <-- публикация + сохранение в NVS

    historyFlashLoop();
    tempHumidityLoop();
}

