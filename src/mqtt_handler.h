#pragma once
#include <Arduino.h>

// MQTT работает в отдельной FreeRTOS-задаче (ядро 0, вместе с Modbus) --
// та же причина, что и у Modbus: s_mqtt.connect() блокирующий на уровне
// TCP-сокета, и при недоступном брокере (пропало электричество/интернет
// на стороне брокера) попытка подключения может зависать надолго. Если
// бы это выполнялось в основном loop() (там же, где веб-сервер) -- весь
// веб-интерфейс был бы недоступен всё это время. Задача изолирует это.
void mqttTaskStart();

// Публикация -- тонкий producer, можно звать из ЛЮБОЙ задачи (в т.ч. из
// основного loop()): просто кладёт запрос в очередь, реальную отправку
// (с учётом enum_map/bit_map/is_switch) делает сама MQTT-задача.
void mqttPublishValue(const String &name, float value, const String &unit);

bool mqttIsConnected();

// Краткий код состояния для веб/логов:
// "ok" | "no_host" | "no_wifi" | "connecting" | "fail"
const char *mqttStateCode();
// Число неудачных попыток подряд (0 если подключены).
int mqttFailCount();

// Вызвать после изменения конфига (список регистров), чтобы переотправить
// Home Assistant discovery payloads под новый набор точек.
void mqttResetDiscovery();

// Приостановить/возобновить MQTT-задачу -- вызывать вокруг замены
// g_config целиком (та же логика, что у modbusTaskPause/Resume).
void mqttTaskPause();
void mqttTaskResume();
