#pragma once
#include <Arduino.h>

// Локальный датчик температуры/влажности SHT41 (I2C) -- измеряет
// условия в месте установки ESP32/инвертора, не связан с Modbus вообще.
// Публикуется через тот же MQTT/history пайплайн, что и обычные
// регистры -- как "виртуальные" записи с is_local=true в g_config.registers
// (см. config.cpp: AmbientTemperature/AmbientHumidity).

void tempHumiditySetup();
void tempHumidityLoop(); // вызывать каждую итерацию main loop()
