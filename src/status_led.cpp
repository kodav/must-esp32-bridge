#include "status_led.h"
#include "config.h"
#include "mqtt_handler.h"
#include "modbus_poll.h"
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel *s_pixel = nullptr;
static bool s_otaActive = false;
static bool s_apMode = false;

// Цвета (R,G,B), яркость применяется отдельно через setBrightness()
static const uint32_t COLOR_OTA      = 0xA000FF; // фиолетовый
static const uint32_t COLOR_AP       = 0xFFC000; // жёлтый
static const uint32_t COLOR_WIFI_CN  = 0x0040FF; // синий
static const uint32_t COLOR_MQTT_BAD = 0xFF6000; // оранжевый
static const uint32_t COLOR_MODBUS_BAD = 0xFF0000; // красный
static const uint32_t COLOR_OK       = 0x00C000; // зелёный
static const uint32_t COLOR_OFF      = 0x000000;

void statusLedSetup() {
  if (!g_config.rgb_led_enabled) return;
  if (s_pixel) { delete s_pixel; s_pixel = nullptr; }
  s_pixel = new Adafruit_NeoPixel(1, g_config.rgb_led_pin, NEO_GRB + NEO_KHZ800);
  s_pixel->begin();
  s_pixel->setBrightness(constrain(g_config.rgb_led_brightness, 0, 255));
  s_pixel->setPixelColor(0, COLOR_OFF);
  s_pixel->show();
}

void statusLedSetOtaActive(bool active) { s_otaActive = active; }
void statusLedSetApMode(bool apMode) { s_apMode = apMode; }

static void setColor(uint32_t rgb) {
  if (!s_pixel) return;
  s_pixel->setPixelColor(0, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
  s_pixel->show();
}

void statusLedLoop() {
  if (!g_config.rgb_led_enabled || !s_pixel) return;

  unsigned long now = millis();

  // --- OTA: быстрая пульсация яркости, приоритет выше всего остального ---
  if (s_otaActive) {
    float phase = (now % 400) / 400.0f; // период 400мс
    float b = 0.3f + 0.7f * fabsf(phase - 0.5f) * 2.0f; // треугольная волна 0.3..1.0
    uint8_t r = (uint8_t)(((COLOR_OTA >> 16) & 0xFF) * b);
    uint8_t g = (uint8_t)(((COLOR_OTA >> 8) & 0xFF) * b);
    uint8_t bl = (uint8_t)((COLOR_OTA & 0xFF) * b);
    s_pixel->setPixelColor(0, r, g, bl);
    s_pixel->show();
    return;
  }

  // --- AP-режим настройки: быстрое мигание жёлтым ---
  if (s_apMode) {
    bool on = (now % 400) < 200;
    setColor(on ? COLOR_AP : COLOR_OFF);
    return;
  }

  // --- WiFi ещё подключается ---
  if (WiFi.status() != WL_CONNECTED) {
    bool on = (now % 600) < 300;
    setColor(on ? COLOR_WIFI_CN : COLOR_OFF);
    return;
  }

  // --- WiFi есть, MQTT недоступен ---
  if (!mqttIsConnected()) {
    bool on = (now % 800) < 400;
    setColor(on ? COLOR_MQTT_BAD : COLOR_OFF);
    return;
  }

  // --- Инвертор не отвечает по Modbus ---
  if (!modbusIsHealthy()) {
    bool on = (now % 300) < 150; // мигает быстрее MQTT-проблемы -- визуально отличимо
    setColor(on ? COLOR_MODBUS_BAD : COLOR_OFF);
    return;
  }

  // --- Всё в порядке: ровный зелёный с редким "вдохом" раз в 4 секунды ---
  unsigned long cyclePos = now % 4000;
  float b = (cyclePos < 200) ? (1.0f - (cyclePos / 200.0f) * 0.6f)
          : (cyclePos > 3800) ? (0.4f + ((cyclePos - 3800) / 200.0f) * 0.6f)
          : 0.4f;
  uint8_t g = (uint8_t)(((COLOR_OK >> 8) & 0xFF) * b);
  s_pixel->setPixelColor(0, 0, g, 0);
  s_pixel->show();
}
