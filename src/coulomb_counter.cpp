#include "coulomb_counter.h"
#include "config.h"
#include "log_buffer.h"
#include "mqtt_handler.h"
#include "history.h"
#include "history_flash.h"
#include <Preferences.h>
#include <math.h>

// ----------------------------- Константы -----------------------------
static const int      kMovingAvgN        = 5;
static const uint32_t kMaxSampleGapMs    = 30000;
static const uint32_t kSavePeriodMs      = 60000;
static const uint32_t kPublishPeriodMs   = 30000;  // редкая публикация по таймеру (раз в 30с)
static const float    kChargeEfficiency  = 0.98f;
static const float    kSaveDeltaAh       = 0.5f;   // сохранение в NVS при сдвиге на 0.5 Ah
static const float    kPublishDeltaSoc   = 0.5f;   // публикация вне очереди при изменении SoC на 0.5%
static const float    kCurrentDeadzone   = 0.2f;   // мертвая зона тока (±0.2А считаем за 0 для отсечения шума)

// ----------------------------- Состояние -----------------------------
static Preferences s_prefs;

static float s_capacityAh    = 100.0f;
static float s_remainingAh   = -1.0f;
static float s_ahCharged     = 0.0f;
static float s_ahDischarged  = 0.0f;

static float s_voltage       = 0.0f;
static float s_lowCutoffV    = 21.0f;
static int   s_chargingState = 0;      // текущая стадия из 15203
static bool  s_haveVoltage   = false;

static float s_avgBuf[kMovingAvgN] = {0};
static int   s_avgCount = 0;
static int   s_avgPos   = 0;
static float s_filtCurrent = 0.0f;

static uint32_t s_lastSampleMs  = 0;
static bool     s_haveSample    = false;

static uint32_t s_lastSaveMs    = 0;
static uint32_t s_lastPublishMs = 0;
static float    s_lastSavedAh   = -1.0f;
static float    s_lastPubSoc    = -1.0f;

static void publishVirtual(const char *name, float value, const char *unit) {
  mqttPublishValue(String(name), value, String(unit));
  historyPush(String(name), value);
  historyFlashPush(String(name), value);
}

void coulombCounterResetFull() {
  s_remainingAh = s_capacityAh;
  s_lastSavedAh = s_remainingAh;
  logPrintf("[coulomb] калибровка 100%%: емкость установлена в %.1f Ah", s_remainingAh);
  publishVirtual("BatterySoC", 100.0f, "%");
  publishVirtual("BatteryAhRemaining", s_remainingAh, "Ah");
}

static void saveToNvs() {
  if (!s_prefs.begin("coulomb", false)) return;
  s_prefs.putFloat("remAh", s_remainingAh);
  s_prefs.putFloat("chgAh", s_ahCharged);
  s_prefs.putFloat("disAh", s_ahDischarged);
  s_prefs.end();
  s_lastSavedAh = s_remainingAh;
}

void coulombCounterBegin() {
  if (s_prefs.begin("coulomb", true)) {
    s_remainingAh  = s_prefs.getFloat("remAh", -1.0f);
    s_ahCharged    = s_prefs.getFloat("chgAh", 0.0f);
    s_ahDischarged = s_prefs.getFloat("disAh", 0.0f);
    s_prefs.end();
  }

  if (s_remainingAh < 0.0f) {
    s_remainingAh = s_capacityAh * 0.5f;
    logPrintf("[coulomb] NVS чист: инициализация 50%% (%.1f Ah). Ждём стадию Float.", s_remainingAh);
  }
  s_lastSavedAh = s_remainingAh;
}

void coulombCounterFeed(const String &name, float value) {
  uint32_t now = millis();

  if (name == "BatteryAh") {
    if (value >= 5.0f && fabsf(value - s_capacityAh) > 0.1f) {
      s_capacityAh = value;
      if (s_remainingAh > s_capacityAh) s_remainingAh = s_capacityAh;
    }
    return;
  }

  if (name == "BatteryLowVoltage") {
    if (value >= 18.0f) s_lowCutoffV = value;
    return;
  }

  if (name == "InverterBatteryVoltage") {
    s_voltage = value;
    s_haveVoltage = true;
    if (s_voltage <= s_lowCutoffV && s_filtCurrent < 0.0f && s_remainingAh > 0.5f) {
      s_remainingAh = 0.0f;
      logPrintf("[coulomb] отсечка по нижнему порогу U=%.1fV -> SoC 0%%", s_voltage);
    }
    return;
  }

  if (name == "ChargingState") {
    s_chargingState = (int)lroundf(value);
    // Калибруем на 100% только если инвертор в Float/Absorb И идет реальный заряд (ток < 0)
    if (s_chargingState >= 1 && s_chargingState <= 3 && s_filtCurrent < 0.0f) {
      if (s_remainingAh < s_capacityAh) {
        coulombCounterResetFull();
      }
    }
    return;
  }
  
  // BattCurrent (25274): на вашем инверторе ПЛЮС = разряд, МИНУС = заряд
  if (name != "BattCurrent") return;

  // Скользящее среднее тока
  s_avgBuf[s_avgPos] = value;
  s_avgPos = (s_avgPos + 1) % kMovingAvgN;
  if (s_avgCount < kMovingAvgN) s_avgCount++;

  float sum = 0.0f;
  for (int i = 0; i < s_avgCount; i++) sum += s_avgBuf[i];
  s_filtCurrent = sum / (float)s_avgCount; // Положительный при разряде

  // Мертвая зона для шума датчика в покое
  float calcCurrent = s_filtCurrent;
  if (fabsf(calcCurrent) < kCurrentDeadzone) {
    calcCurrent = 0.0f;
  }

  // Интегрирование А·ч с учетом правильного знака (плюс = разряд)
  if (s_haveSample) {
    uint32_t dtMs = now - s_lastSampleMs;
    if (dtMs > 0 && dtMs <= kMaxSampleGapMs) {
      float dtHours = (float)dtMs / 3600000.0f;
      
      // Инвертируем знак для кулонометра: разряд (+) уменьшает остаток
      float effectiveCurrent = -calcCurrent; 
      float eff = (effectiveCurrent > 0.0f) ? kChargeEfficiency : 1.0f; 
      float dAh = effectiveCurrent * eff * dtHours;

      s_remainingAh += dAh;

      if (effectiveCurrent > 0.0f) s_ahCharged += dAh;
      else                         s_ahDischarged += (-dAh);

      if (s_remainingAh < 0.0f) s_remainingAh = 0.0f;
      if (s_remainingAh > s_capacityAh) s_remainingAh = s_capacityAh;
    }
  }

  s_lastSampleMs = now;
  s_haveSample = true;
}

void coulombCounterLoop() {
  uint32_t now = millis();

  // Если батарея в Float, принудительно держим 100%
  if (s_chargingState == 2 && s_remainingAh < s_capacityAh) {
    s_remainingAh = s_capacityAh;
  }

  bool deltaReached = fabsf(s_remainingAh - s_lastSavedAh) >= kSaveDeltaAh;
  if (deltaReached && (now - s_lastSaveMs >= kSavePeriodMs)) {
    saveToNvs();
    s_lastSaveMs = now;
  }

  float soc = coulombCounterSoC();
  bool socChanged = (s_lastPubSoc < 0.0f) || (fabsf(soc - s_lastPubSoc) >= kPublishDeltaSoc);

  if (socChanged || (now - s_lastPublishMs >= kPublishPeriodMs)) {
    s_lastPublishMs = now;
    s_lastPubSoc    = soc;

    publishVirtual("BatterySoC", soc, "%");
    publishVirtual("BatteryAhRemaining", s_remainingAh, "Ah");

    if (s_haveVoltage) {
      publishVirtual("BatteryPowerCalc", s_voltage * s_filtCurrent, "W");
    }
  }
}

float coulombCounterSoC() {
  if (s_capacityAh <= 0.0f) return 0.0f;
  float soc = (s_remainingAh / s_capacityAh) * 100.0f;
  if (soc < 0.0f) soc = 0.0f;
  if (soc > 100.0f) soc = 100.0f;
  return soc;
}

float coulombCounterAhRemaining() {
  return s_remainingAh;
}

float coulombCounterPower() {
  return s_voltage * s_filtCurrent;
}