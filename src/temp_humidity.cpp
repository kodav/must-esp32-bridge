#include "temp_humidity.h"
#include "config.h"
#include "log_buffer.h"
#include "mqtt_handler.h"
#include "history.h"
#include <Wire.h>
#include <Adafruit_SHT4x.h>

// ЧЕСТНО: API Adafruit_SHT4x приведён по типовому паттерну библиотек
// Adafruit на базе Adafruit_Sensor (getEvent/sensors_event_t) -- не
// проверено сборкой на реальном железе. Если сигнатура/имена методов
// разойдутся с установленной версией библиотеки, компилятор укажет
// точную строку с ошибкой -- поправить можно без переписывания
// остальной логики модуля (историю/MQTT-публикацию это не затронет).

static Adafruit_SHT4x s_sht4;
static bool s_ready = false;
static unsigned long s_lastRead = 0;
static const unsigned long READ_INTERVAL_MS = 30000; // раз в 30с -- температура/влажность меняются медленно

void tempHumiditySetup() {
  if (!g_config.sht41_enabled) {
    logPrintln("[sht41] отключён в конфиге");
    return;
  }

  Wire.begin(g_config.sht41_sda_pin, g_config.sht41_scl_pin);

  if (!s_sht4.begin(&Wire)) {
    logPrintln("[sht41] датчик не найден на шине I2C -- проверьте подключение SDA/SCL/питание");
    return;
  }

  s_sht4.setPrecision(SHT4X_HIGH_PRECISION);
  s_sht4.setHeater(SHT4X_NO_HEATER);

  s_ready = true;
  logPrintf("[sht41] датчик найден и готов (SDA=%d, SCL=%d)", g_config.sht41_sda_pin, g_config.sht41_scl_pin);
}

void tempHumidityLoop() {
  if (!s_ready) return;

  unsigned long now = millis();
  if (now - s_lastRead < READ_INTERVAL_MS) return;
  s_lastRead = now;

  sensors_event_t humidity, temp;
  if (!s_sht4.getEvent(&humidity, &temp)) {
    logPrintln("[sht41] ошибка чтения датчика");
    return;
  }

  float tempC = temp.temperature;
  float rh = humidity.relative_humidity;

  logPrintf("[data] AmbientTemperature = %.2f C, AmbientHumidity = %.2f %%", tempC, rh);

  mqttPublishValue("AmbientTemperature", tempC, "°C");
  mqttPublishValue("AmbientHumidity", rh, "%");
  historyPush("AmbientTemperature", tempC);
  historyPush("AmbientHumidity", rh);
}
