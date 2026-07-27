#include "history.h"
#include "config.h"
#include "log_buffer.h"
#include <esp_heap_caps.h>
#include <time.h>

struct __attribute__((packed)) HistoryEntry {
  uint32_t timestamp; // unix-время (секунды), см. примечание в history.h
  uint16_t regIndex;
  float value;
};

static HistoryEntry *s_buffer = nullptr;
static size_t s_capacity = 0;
static size_t s_writeIdx = 0;
static uint32_t s_totalWrites = 0; // монотонный счётчик -- переполнение практически невозможно за время жизни устройства

static void allocateBuffer() {
  if (s_buffer) {
    heap_caps_free(s_buffer);
    s_buffer = nullptr;
  }
  s_writeIdx = 0;
  s_totalWrites = 0;
  s_capacity = 0;

  if (!g_config.history_enabled || g_config.registers.empty()) {
    logPrintln("[history] отключена в конфиге");
    return;
  }

  float hours = g_config.history_hours;
  if (hours <= 0) hours = 1;
  if (hours > 72) hours = 72; // защита от случайной гигантской аллокации

  double readingsPerHour = (3600000.0 / (double)g_config.poll_interval_ms) * g_config.registers.size();
  size_t capacity = (size_t)(readingsPerHour * hours);
  if (capacity < 100) capacity = 100;

  size_t bytesNeeded = capacity * sizeof(HistoryEntry);
  s_buffer = (HistoryEntry *)heap_caps_malloc(bytesNeeded, MALLOC_CAP_SPIRAM);

  if (!s_buffer) {
    logPrintf("[history] не удалось выделить %u байт в PSRAM, история отключена", (unsigned)bytesNeeded);
    return;
  }

  s_capacity = capacity;
  logPrintf("[history] буфер: %.1f ч ~= %u записей (%.1f KB в PSRAM)",
            hours, (unsigned)capacity, bytesNeeded / 1024.0f);
}

void historySetup() {
  allocateBuffer();
}

void historyReconfigure() {
  allocateBuffer();
}

void historyPush(const String &name, float value) {
  if (!s_buffer) return;

  int regIndex = -1;
  for (size_t i = 0; i < g_config.registers.size(); i++) {
    if (g_config.registers[i].name == name) { regIndex = (int)i; break; }
  }
  if (regIndex < 0) return;

  // Если NTP ещё не синхронизировался, time(nullptr) вернёт что-то около 1970
  // года -- сознательно не подменяем это на millis(), чтобы не путать
  // "настоящее" и "приблизительное" время в одном поле разными способами.
  // Фронтенд сам увидит неправдоподобный год и покажет предупреждение.
  uint32_t ts = (uint32_t)time(nullptr);

  s_buffer[s_writeIdx] = {ts, (uint16_t)regIndex, value};
  s_writeIdx = (s_writeIdx + 1) % s_capacity;
  s_totalWrites++;
}

String historyGetJson(const String &registerName, size_t maxPoints) {
  if (!s_buffer || maxPoints == 0) return "{\"points\":[]}";

  int regIndex = -1;
  for (size_t i = 0; i < g_config.registers.size(); i++) {
    if (g_config.registers[i].name == registerName) { regIndex = (int)i; break; }
  }
  if (regIndex < 0) return "{\"points\":[]}";

  size_t validCount = (s_totalWrites < s_capacity) ? s_totalWrites : s_capacity;
  size_t oldestIdx = (s_totalWrites < s_capacity) ? 0 : s_writeIdx;

  // Первый проход -- считаем, сколько записей вообще относится к этому регистру
  size_t matchCount = 0;
  for (size_t i = 0; i < validCount; i++) {
    size_t idx = (oldestIdx + i) % s_capacity;
    if (s_buffer[idx].regIndex == (uint16_t)regIndex) matchCount++;
  }
  if (matchCount == 0) return "{\"points\":[]}";

  size_t stride = matchCount / maxPoints;
  if (stride < 1) stride = 1;

  String out = "{\"points\":[";
  bool first = true;
  size_t matchSeen = 0;
  for (size_t i = 0; i < validCount; i++) {
    size_t idx = (oldestIdx + i) % s_capacity;
    if (s_buffer[idx].regIndex != (uint16_t)regIndex) continue;
    matchSeen++;
    if ((matchSeen - 1) % stride != 0) continue;

    if (!first) out += ",";
    first = false;
    out += "{\"t\":" + String(s_buffer[idx].timestamp) + ",\"v\":" + String(s_buffer[idx].value, 3) + "}";
  }
  out += "]}";
  return out;
}
