#pragma once
#include <Arduino.h>
#include <functional>

using RegisterValueCallback = std::function<void(const String &name, float value, const String &unit)>;

// Запускает отдельную FreeRTOS-задачу опроса Modbus на ЯДРЕ 0 -- вызывать
// один раз в setup(). Веб-сервер и всё остальное (обычный Arduino loop(),
// ядро 1) выполняются физически параллельно, независимо от Modbus --
// опрос больше не может подвесить веб-интерфейс вообще, даже теоретически.
void modbusTaskStart();

// Вызывать каждую итерацию main loop() -- неблокирующе вычитывает уже
// готовые результаты из очереди, которую заполняет фоновая задача.
void modbusPollLoop(const RegisterValueCallback &onValue);

bool modbusIsHealthy();

// Приостановить/возобновить задачу опроса. ОБЯЗАТЕЛЬНО вызывать вокруг
// любой замены g_config целиком (см. web_server.cpp) -- задача в фоне
// читает g_config.registers, и если конфиг переприсвоят прямо во время
// её работы, это гонка данных с риском краша. Простая приостановка на
// время замены конфига дешевле и надёжнее, чем городить мьютекс вокруг
// g_config во всех местах, где он используется по всему проекту.
void modbusTaskPause();
void modbusTaskResume();

// Результат попытки записи регистра.
struct ModbusWriteResult {
  bool success;
  uint8_t errorCode; // код ошибки Modbus (0 если success==true)
};

// Записать ОДИН 16-битный регистр (function 06, Write Single Register) и
// ДОЖДАТЬСЯ реального результата -- запрос ставится в очередь фоновой
// задаче (только она владеет RS485-шиной), эта функция блокируется (не
// дольше timeoutMs) в ожидании фактического ответа устройства. Вызывать
// из web-обработчика -- блокировка на секунды в редком, осознанном,
// пользователем инициированном запросе приемлема (в отличие от обычного
// опроса, который не должен блокировать вообще).
// rawValue -- уже В СЫРОМ ВИДЕ (после деления желаемого значения на
// scale регистра, см. web_server.cpp).
ModbusWriteResult modbusWriteRegisterSync(uint16_t address, uint16_t rawValue, uint32_t timeoutMs = 3000);
