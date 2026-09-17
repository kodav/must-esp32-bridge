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
static const float    kChargeEfficiency  = 0.98f;
static const float    kSaveDeltaAh       = 0.5f;   // сохранение в NVS при сдвиге на 0.5 Ah
static const float    kPublishDeltaSoc   = 0.5f;   // публикация вне очереди при изменении SoC на 0.5%
static const float    kCurrentDeadzone   = 0.05f;   // мертвая зона тока (±0.05А считаем за 0 для отсечения шума)
// Вместо статической константы kPublishPeriodMs используем переменную
static uint32_t s_publishPeriodMs = 15000; // По умолчанию, если конфиг не подгрузился

// ----------------------------- Константы -----------------------------
static const float    kFloatCalibrateCurrentA = 1.0f;   // |I| < 1 A — батарея полна
static const float    kFloatCalibrateVoltageMargin = 0.2f; // допуск по напряжению
static const uint32_t kFullCalibrateHoldMs = 60000;     // условие должно держаться 60 с

// ----------------------------- Состояние -----------------------------
static float    s_floatVoltage   = 27.0f;   // из конфига (FloatVoltage)
static float    s_absorbVoltage  = 28.4f;   // из конфига (AbsorptionVoltage)
static uint32_t s_fullCondStartMs = 0;      // когда условие калибровки стало истинным
static bool     s_fullCondActive  = false;  // условие калибровки активно

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

static void saveToNvs() {
  if (!s_prefs.begin("coulomb", false)) return;
  s_prefs.putFloat("remAh", s_remainingAh);
  s_prefs.putFloat("chgAh", s_ahCharged);
  s_prefs.putFloat("disAh", s_ahDischarged);
  s_prefs.end();
  s_lastSavedAh = s_remainingAh;
}

void coulombCounterResetFull() {
  s_remainingAh = s_capacityAh;
  s_lastSavedAh = s_remainingAh;
  saveToNvs();
  logPrintf("[coulomb] калибровка 100%%: емкость установлена в %.1f Ah", s_remainingAh);
  publishVirtual("BatterySoC", 100.0f, "%");
  publishVirtual("BatteryAhRemaining", s_remainingAh, "Ah");
}

void coulombCounterBegin() {
  // Берем актуальный интервал опроса из глобального конфига (в миллисекундах)
  s_publishPeriodMs = g_config.poll_interval_ms;

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

// Возвращает true, если батарея действительно полна по совокупности признаков
static bool isBatteryFullCondition() {
    // 1. Стадия заряда должна быть Float (2) или Absorb (3)
    if (s_chargingState != 2 && s_chargingState != 3) return false;

    // 2. Ток заряда должен быть мал (батарея почти не принимает ток)
    //    s_filtCurrent < 0 — заряд; берём модуль
    if (fabsf(s_filtCurrent) > kFloatCalibrateCurrentA) return false;

    // 3. Напряжение должно быть близко к уставке поддержки/абсорбции
    float vTarget = (s_chargingState == 2) ? s_floatVoltage : s_absorbVoltage;
    if (!s_haveVoltage) return false;
    if (s_voltage < vTarget - kFloatCalibrateVoltageMargin) return false;

    // 4. Уже не 100% — иначе нечего калибровать
    if (s_remainingAh >= s_capacityAh - 0.01f) return false;

    return true;
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

  if (name == "FloatVoltage") {
      if (value >= 20.0f && value <= 32.0f) s_floatVoltage = value;
      return;
  }
  if (name == "AbsorptionVoltage") {
      if (value >= 20.0f && value <= 32.0f) s_absorbVoltage = value;
      return;
  }

  if (name == "InverterBatteryVoltage") {
    s_voltage = value;
    s_haveVoltage = true;
    if (s_voltage <= s_lowCutoffV - 0.2f && s_filtCurrent > 0.5f && s_remainingAh > 0.5f) {
        s_remainingAh = 0.0f;
        logPrintf("[coulomb] отсечка по нижнему порогу U=%.1fV I=%.2fA -> SoC 0%%",
                  s_voltage, s_filtCurrent);
    }
    return;
  }

  if (name == "ChargingState") {
      s_chargingState = (int)lroundf(value);
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

  // Интегрирование А·ч (calcCurrent: плюс = разряд, минус = заряд)
  if (s_haveSample) {
    uint32_t dtMs = now - s_lastSampleMs;
    if (dtMs > 0 && dtMs <= kMaxSampleGapMs) {
      float dtHours = (float)dtMs / 3600000.0f;
      
      float effectiveCurrent = calcCurrent;
      float eff = 1.0f;

      if (effectiveCurrent > 0.0f) {
        // Разряд: учитываем потери и эффект Пёкерта при высоких токах
        // Чем выше ток, тем больше энергии теряется в тепле -> быстрее падает SoC
        if (effectiveCurrent > 40.0f) {
          eff = 1.12f; // Тяжелая нагрузка (>0.4C)
        } else if (effectiveCurrent > 15.0f) {
          eff = 1.06f; // Средняя нагрузка
        } else {
          eff = 1.02f; // Малая нагрузка
        }
      } else {
        // Заряд: используем коэффициент эффективности заряда
        eff = kChargeEfficiency;
      }

      // Разряд (положительный ток) уменьшает остаток емкости
      float dAh = effectiveCurrent * eff * dtHours;
      s_remainingAh -= dAh;

      if (effectiveCurrent > 0.0f) {
        s_ahDischarged += dAh;
      } else {
        s_ahCharged += (-dAh);
      }

      if (s_remainingAh < 0.0f) s_remainingAh = 0.0f;
      if (s_remainingAh > s_capacityAh) s_remainingAh = s_capacityAh;
    }
  }
  
  s_lastSampleMs = now;
  s_haveSample = true;
}

void coulombCounterLoop() {
  uint32_t now = millis();

// ---- Калибровка 100% с гистерезисом по времени ----
  bool cond = isBatteryFullCondition();
  if (cond) {
      if (!s_fullCondActive) {
          s_fullCondActive  = true;
          s_fullCondStartMs = now;
      } else if (now - s_fullCondStartMs >= kFullCalibrateHoldMs) {
          // Условие держится 60 секунд — калибруем
          coulombCounterResetFull();
          s_fullCondActive  = false;
          s_fullCondStartMs = 0;
          logPrintf("[coulomb] калибровка 100%%: U=%.2fV I=%.2fA state=%d",
                    s_voltage, s_filtCurrent, s_chargingState);
      }
  } else {
      // Условие нарушилось — сбрасываем таймер
      s_fullCondActive  = false;
      s_fullCondStartMs = 0;
  }
    
  bool deltaReached = fabsf(s_remainingAh - s_lastSavedAh) >= kSaveDeltaAh;
  if (deltaReached && (now - s_lastSaveMs >= kSavePeriodMs)) {
    saveToNvs();
    s_lastSaveMs = now;
  }

  float soc = coulombCounterSoC();
  bool socChanged = (s_lastPubSoc < 0.0f) || (fabsf(soc - s_lastPubSoc) >= kPublishDeltaSoc);

  // Используем s_publishPeriodMs вместо константы
  if (socChanged || (now - s_lastPublishMs >= s_publishPeriodMs)) {
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