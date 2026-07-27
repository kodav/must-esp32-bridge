#pragma once
#include <Arduino.h>

// Простой лог: пишет в Serial (для отладки по UART) и хранит последние
// N строк в кольцевом буфере в памяти. Веб-страница опрашивает их
// поллингом (GET /api/logs?since=N) -- проще и надёжнее, чем SSE,
// работает с обычным синхронным WebServer без доп. библиотек.

void logInit();
void logPrintf(const char *fmt, ...);
void logPrintln(const String &s);

// Перехватывает ВСЕ системные ESP_LOG-сообщения (из компонентов ESP-IDF --
// USB Host, WiFi, TLS и т.д., не только наши собственные logPrintln) и
// добавляет их в тот же кольцевой буфер, что виден в веб-интерфейсе.
// Вызывать один раз, максимально рано в setup() -- до этого вызова такие
// сообщения видны только в Serial Monitor, но не в вебе.
void logInstallEspLogCapture();

// Отдаёт JSON вида {"next":123,"lines":["...","..."]}
// since -- id последней строки, которую клиент уже видел (0 при первом запросе)
String logGetJson(uint32_t since);
