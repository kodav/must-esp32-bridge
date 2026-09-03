#pragma once
#include <Arduino.h>

void coulombCounterBegin();
void coulombCounterFeed(const String &name, float value);
void coulombCounterLoop();

float coulombCounterSoC();          // текущий заряд в % (0..100)
float coulombCounterAhRemaining();  // остаток ёмкости в А·ч
float coulombCounterPower();        // расчётная мощность P = U * I, Вт
void coulombCounterResetFull();     // ручной сброс на 100% (для Web/MQTT команд)
