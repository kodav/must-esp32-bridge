#pragma once
#include <Arduino.h>

void mqttSetup();
void mqttLoop();
bool mqttIsConnected();
void mqttPublishValue(const String &name, float value, const String &unit);

// Вызвать после изменения конфига (список регистров), чтобы переотправить
// Home Assistant discovery payloads под новый набор точек.
void mqttResetDiscovery();
