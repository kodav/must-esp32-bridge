#pragma once
#include <Arduino.h>

// RS485/UART -- единственный поддерживаемый транспорт (USB Host убран
// из проекта, см. README).

// Возвращает готовый к использованию Stream для Modbus RTU.
Stream *modbusTransportBegin();

// true, если используется явный DE/RE пин управления направлением RS485
// (для ModbusMaster preTransmission/postTransmission). false для модулей
// с автонаправлением (DE/RE не настроен, uart_de_pin = -1 в конфиге).
bool modbusTransportNeedsDirectionControl();
void modbusTransportSetTx();
void modbusTransportSetRx();
