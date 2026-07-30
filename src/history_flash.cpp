#include "history_flash.h"
#include "config.h"
#include "log_buffer.h"
#include <LittleFS.h>
#include <vector>
#include <time.h>

static const char *FLASH_PATH = "/history_flash.bin";
static const uint32_t FLASH_MAGIC = 0x4D505631; // "MPV1"

// Фиксированная ёмкость (не зависит от числа регистров, в отличие от
// PSRAM-буфера) -- бюджет ~1.5MB из 3MB раздела spiffs, оставляя запас
// под config.json и служебные накладные расходы LittleFS/wear-leveling.
static const uint32_t FLASH_HISTORY_CAPACITY = 150000;

struct __attribute__((packed)) FlashHeader {
  uint32_t magic;
  uint32_t capacity;
  uint32_t writeIndex;
  uint32_t totalWrites;
};

struct __attribute__((packed)) FlashEntry {
  uint32_t timestamp;
  uint16_t regIndex;
  float value;
};

struct LastValue {
  bool valid; // БЕЗ значения по умолчанию -- та же причина, что у writable/enum_map
              // в config.h: default member initializer ломает агрегатную
              // инициализацию структуры на некоторых толчейнах/стандартах C++
  float value;
};

static bool s_enabled = false;
static uint32_t s_writeIndex = 0;
static uint32_t s_totalWrites = 0;
static unsigned long s_lastCheckpoint = 0;
static std::vector<LastValue> s_lastValues;

static bool openOrCreate() {
  if (LittleFS.exists(FLASH_PATH)) {
    File f = LittleFS.open(FLASH_PATH, "r+");
    if (f) {
      FlashHeader hdr;
      bool valid = (f.read((uint8_t *)&hdr, sizeof(hdr)) == sizeof(hdr)) &&
                   hdr.magic == FLASH_MAGIC && hdr.capacity == FLASH_HISTORY_CAPACITY;
      f.close();
      if (valid) {
        s_writeIndex = hdr.writeIndex;
        s_totalWrites = hdr.totalWrites;
        return true;
      }
      logPrintln("[flash-history] существующий файл повреждён/старого формата -- пересоздаю");
    }
  }

  File f = LittleFS.open(FLASH_PATH, "w");
  if (!f) return false;
  FlashHeader hdr{FLASH_MAGIC, FLASH_HISTORY_CAPACITY, 0, 0};
  f.write((uint8_t *)&hdr, sizeof(hdr));
  f.close();
  s_writeIndex = 0;
  s_totalWrites = 0;
  return true;
}

static void writeCheckpoint() {
  if (!s_enabled) return;

  File f = LittleFS.open(FLASH_PATH, "r+");
  if (!f) {
    logPrintln("[flash-history] не удалось открыть файл для записи снимка");
    return;
  }

  uint32_t ts = (uint32_t)time(nullptr);
  size_t written = 0;

  for (size_t i = 0; i < s_lastValues.size(); i++) {
    if (!s_lastValues[i].valid) continue;

    FlashEntry e{ts, (uint16_t)i, s_lastValues[i].value};
    size_t offset = sizeof(FlashHeader) + (size_t)s_writeIndex * sizeof(FlashEntry);
    f.seek(offset);
    f.write((uint8_t *)&e, sizeof(e));

    s_writeIndex = (s_writeIndex + 1) % FLASH_HISTORY_CAPACITY;
    s_totalWrites++;
    written++;
  }

  FlashHeader hdr{FLASH_MAGIC, FLASH_HISTORY_CAPACITY, s_writeIndex, s_totalWrites};
  f.seek(0);
  f.write((uint8_t *)&hdr, sizeof(hdr));
  f.close();

  logPrintf("[flash-history] снимок записан: %u регистров, всего снимков с начала: %u",
            (unsigned)written, (unsigned)(s_totalWrites / (written ? written : 1)));
}

void historyFlashSetup() {
  s_enabled = g_config.flash_history_enabled;
  if (!s_enabled) {
    logPrintln("[flash-history] отключена в конфиге");
    return;
  }

  LittleFS.begin(false); // обычно уже смонтирована config.cpp -- на всякий случай
  s_lastValues.assign(g_config.registers.size(), LastValue{});

  if (!openOrCreate()) {
    logPrintln("[flash-history] не удалось открыть/создать файл -- отключена");
    s_enabled = false;
    return;
  }

  uint32_t used = (s_totalWrites < FLASH_HISTORY_CAPACITY) ? s_totalWrites : FLASH_HISTORY_CAPACITY;
  logPrintf("[flash-history] готова: %u/%u записей уже накоплено, интервал снимков %d мин",
            (unsigned)used, (unsigned)FLASH_HISTORY_CAPACITY, g_config.flash_history_interval_min);
}

void historyFlashPush(const String &name, float value) {
  if (!s_enabled) return;

  int idx = -1;
  for (size_t i = 0; i < g_config.registers.size(); i++) {
    if (g_config.registers[i].name == name) { idx = (int)i; break; }
  }
  if (idx < 0) return;
  if ((size_t)idx >= s_lastValues.size()) s_lastValues.resize(g_config.registers.size());

  s_lastValues[idx].valid = true;
  s_lastValues[idx].value = value;
}

void historyFlashLoop() {
  if (!s_enabled) return;

  unsigned long intervalMs = (unsigned long)g_config.flash_history_interval_min * 60000UL;
  if (intervalMs == 0) intervalMs = 60000UL; // защита от опечатки "0 минут" в конфиге

  unsigned long now = millis();
  if (now - s_lastCheckpoint < intervalMs) return;
  s_lastCheckpoint = now;

  writeCheckpoint();
}

void historyFlashReconfigure() {
  bool wasEnabled = s_enabled;
  s_enabled = g_config.flash_history_enabled;
  s_lastValues.assign(g_config.registers.size(), LastValue{});

  if (s_enabled && !wasEnabled) {
    LittleFS.begin(false);
    if (openOrCreate()) {
      logPrintln("[flash-history] включена, файл готов");
    } else {
      logPrintln("[flash-history] не удалось открыть/создать файл -- отключена");
      s_enabled = false;
    }
  } else if (!s_enabled) {
    logPrintln("[flash-history] отключена в конфиге");
  }
}

void historyFlashExportCsv(const std::function<void(const String &)> &onChunk) {
  if (!s_enabled) return;

  File f = LittleFS.open(FLASH_PATH, "r");
  if (!f) return;

  FlashHeader hdr;
  if (f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr) || hdr.magic != FLASH_MAGIC) {
    f.close();
    return;
  }

  size_t validCount = (hdr.totalWrites < hdr.capacity) ? hdr.totalWrites : hdr.capacity;
  size_t oldestIdx = (hdr.totalWrites < hdr.capacity) ? 0 : hdr.writeIndex;

  String chunk;
  chunk.reserve(4200);
  const size_t CHUNK_FLUSH_SIZE = 4096;

  for (size_t i = 0; i < validCount; i++) {
    size_t idx = (oldestIdx + i) % hdr.capacity;
    size_t offset = sizeof(FlashHeader) + idx * sizeof(FlashEntry);
    f.seek(offset);

    FlashEntry e;
    if (f.read((uint8_t *)&e, sizeof(e)) != sizeof(e)) continue;
    if (e.regIndex >= g_config.registers.size()) continue; // конфиг мог поменяться -- пропускаем осиротевшие

    chunk += String(e.timestamp);
    chunk += ",";
    chunk += g_config.registers[e.regIndex].name;
    chunk += ",";
    chunk += String(e.value, 3);
    chunk += "\n";

    if (chunk.length() >= CHUNK_FLUSH_SIZE) {
      onChunk(chunk);
      chunk = "";
    }
  }
  f.close();

  if (chunk.length() > 0) onChunk(chunk);
}
