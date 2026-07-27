#include "log_buffer.h"
#include <stdarg.h>
#include <vector>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Буфер логов теперь пишется из НЕСКОЛЬКИХ задач одновременно: основной
// loop() (ядро 1), фоновая задача опроса Modbus (ядро 0, свои
// диагностические сообщения об ошибках чтения) и системные задачи
// ESP-IDF (WiFi/lwIP и т.д. -- через перехват ESP_LOG). Без мьютекса
// это гонка данных на обычном std::vector -- защищаем.
//
// Рекурсивный мьютекс специально: если внутри pushLine() (уже держим
// мьютекс) что-то в свою очередь дёрнет ESP_LOG на той же задаче
// (например, предупреждение от аллокатора) -- обычный мьютекс тут
// заблокировался бы намертво сам на себя, рекурсивный -- нет.
static SemaphoreHandle_t s_logMutex = nullptr;

static const size_t kMaxLines = 400;
static std::vector<String> s_lines;
static uint32_t s_nextId = 1; // id строки, которая будет добавлена следующей

static void pushLine(const String &s) {
  if (!s_logMutex) return; // logInit() ещё не вызывался -- не должно происходить в норме
  xSemaphoreTakeRecursive(s_logMutex, portMAX_DELAY);
  s_lines.push_back(s);
  if (s_lines.size() > kMaxLines) {
    s_lines.erase(s_lines.begin());
  }
  s_nextId++;
  xSemaphoreGiveRecursive(s_logMutex);
}

void logInit() {
  if (!s_logMutex) {
    s_logMutex = xSemaphoreCreateRecursiveMutex();
  }
  xSemaphoreTakeRecursive(s_logMutex, portMAX_DELAY);
  s_lines.clear();
  s_nextId = 1;
  xSemaphoreGiveRecursive(s_logMutex);
}

void logPrintln(const String &s) {
  Serial.println(s); // Serial сам по себе потокобезопасен на запись у Arduino-ESP32
  pushLine(s);
}

void logPrintf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logPrintln(String(buf));
}

// -----------------------------------------------------------------------
// Перехват системных ESP_LOG (из WiFi, TLS и т.д.) -- уже мог вызываться
// из произвольных системных задач и раньше, теперь это особенно явно
// важно защитить мьютексом, раз у нас появилась ещё и своя фоновая задача.
// -----------------------------------------------------------------------
static vprintf_like_t s_originalVprintf = nullptr;

static int captureVprintf(const char *fmt, va_list args) {
  char buf[256];
  va_list argsCopy;
  va_copy(argsCopy, args);
  vsnprintf(buf, sizeof(buf), fmt, argsCopy);
  va_end(argsCopy);

  String line(buf);
  line.trim(); // убираем завершающий \n, который ESP_LOG всегда добавляет

  // ESP_LOG раскрашивает вывод ANSI escape-кодами для терминала --
  // в вебе это будет просто мусором вида "\x1b[0;33m", вырезаем.
  String clean;
  clean.reserve(line.length());
  bool inEscape = false;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '\033') { inEscape = true; continue; }
    if (inEscape) {
      if (c == 'm') inEscape = false;
      continue;
    }
    clean += c;
  }

  if (clean.length() > 0) {
    pushLine(clean);
  }

  return s_originalVprintf ? s_originalVprintf(fmt, args) : vprintf(fmt, args);
}

void logInstallEspLogCapture() {
  s_originalVprintf = esp_log_set_vprintf(&captureVprintf);

  // ВАЖНО: держите этот уровень не выше INFO -- поднимали до VERBOSE один
  // раз для отладки (давно решённой) проблемы и словили Guru Meditation
  // из-за перегрузки стека одной из системных задач. Если понадобится
  // больше детализации для конкретного компонента -- поднимайте точечно
  // через esp_log_level_set("тег", ...) для одного нужного тега.
  esp_log_level_set("*", ESP_LOG_INFO);
}

String logGetJson(uint32_t since) {
  if (!s_logMutex) return "{\"next\":1,\"lines\":[]}";
  xSemaphoreTakeRecursive(s_logMutex, portMAX_DELAY);

  uint32_t firstStoredId = (s_nextId > s_lines.size()) ? (s_nextId - s_lines.size()) : 1;
  uint32_t startFrom = (since >= firstStoredId) ? since : firstStoredId - 1;

  String out = "{\"next\":" + String(s_nextId) + ",\"lines\":[";
  bool first = true;
  for (size_t i = 0; i < s_lines.size(); i++) {
    uint32_t lineId = firstStoredId + i;
    if (lineId <= startFrom) continue;
    if (!first) out += ",";
    first = false;
    String esc = s_lines[i];
    esc.replace("\\", "\\\\");
    esc.replace("\"", "\\\"");
    esc.replace("\n", " ");
    esc.replace("\r", "");
    out += "\"" + esc + "\"";
  }
  out += "]}";

  xSemaphoreGiveRecursive(s_logMutex);
  return out;
}
