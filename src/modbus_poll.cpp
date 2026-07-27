#include "modbus_poll.h"
#include "modbus_transport.h"
#include "config.h"
#include "log_buffer.h"
#include <ModbusMaster.h>
#include <vector>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ---------------------------------------------------------------------
// Modbus-опрос выполняется в ОТДЕЛЬНОЙ FreeRTOS-задаче на ядре 0.
// Arduino loop() (веб-сервер, OTA, MQTT, статус-индикатор) крутится на
// ядре 1 как обычно и НИКОГДА не блокируется опросом Modbus -- настоящий
// параллелизм на разных ядрах.
//
// Результаты передаются в main loop() через тонкую FreeRTOS-очередь
// (только regIndex + value). mqttPublishValue()/historyPush()/логирование
// живых данных по-прежнему вызываются только из main loop() -- поэтому
// history.cpp и MQTT-клиент не требуют мьютексов.
//
// ГРУППОВОЕ ЧТЕНИЕ: соседние по адресу регистры читаются ОДНИМ Modbus-
// запросом (function 03, диапазон) вместо запроса на каждый регистр --
// у нас 63 регистра обычно укладываются в 5 запросов. Потолок группы --
// 60 слов (не 125 из спецификации Modbus): у библиотеки ModbusMaster
// внутренний буфер ответа физически ограничен 64 словами.
// ---------------------------------------------------------------------

struct ModbusResult {
  uint16_t regIndex;
  float value;
};

struct WriteRequest {
  uint16_t address;
  uint16_t rawValue;
  QueueHandle_t resultQueue; // куда положить результат -- создаётся вызывающей стороной, на один запрос
};

struct ReadGroup {
  uint16_t startAddr;
  uint16_t totalCount; // сколько 16-битных слов читаем одним запросом
  std::vector<uint16_t> members; // индексы в g_config.registers, отсортированы по адресу
};

static const uint16_t MAX_GROUP_WORDS = 60; // с запасом ниже ku8MaxBufferSize=64
static const uint16_t MAX_GROUP_GAP = 10;   // макс. "пустых" слов между соседями внутри группы

static ModbusMaster node;
static QueueHandle_t s_resultQueue = nullptr;
static QueueHandle_t s_writeQueue = nullptr;
static TaskHandle_t s_taskHandle = nullptr;

static volatile bool s_lastReadOk = false;
static volatile bool s_transportOk = false;

static std::vector<ReadGroup> s_groups;
static size_t s_boundRegisterCount = (size_t)-1; // чтобы гарантированно пересобрать группы при первом вызове

static void preTransmission()  { modbusTransportSetTx(); }
static void postTransmission() { modbusTransportSetRx(); }

// Пересобирает группы чтения из текущего g_config.registers -- вызывать
// при изменении списка регистров (обнаруживается по несовпадению размера).
static void rebuildGroups() {
  s_groups.clear();

  std::vector<uint16_t> order(g_config.registers.size());
  for (uint16_t i = 0; i < order.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(), [](uint16_t a, uint16_t b) {
    return g_config.registers[a].address < g_config.registers[b].address;
  });

  for (uint16_t idx : order) {
    const RegisterDef &r = g_config.registers[idx];
    if (r.is_local) continue; // не Modbus-регистр (например, локальный I2C-датчик) -- пропускаем
    uint16_t regEnd = r.address + r.count; // адрес сразу за этим регистром

    if (!s_groups.empty()) {
      ReadGroup &g = s_groups.back();
      uint16_t groupEnd = g.startAddr + g.totalCount;
      uint16_t gap = (r.address >= groupEnd) ? (r.address - groupEnd) : 0;
      uint16_t newTotal = (regEnd > groupEnd) ? (regEnd - g.startAddr) : g.totalCount;

      if (gap <= MAX_GROUP_GAP && newTotal <= MAX_GROUP_WORDS) {
        g.members.push_back(idx);
        g.totalCount = newTotal;
        continue;
      }
    }

    ReadGroup g;
    g.startAddr = r.address;
    g.totalCount = r.count;
    g.members.push_back(idx);
    s_groups.push_back(g);
  }

  uint16_t totalWords = 0;
  for (auto &g : s_groups) totalWords += g.totalCount;
  logPrintf("[modbus] группы чтения пересобраны: %u регистров -> %u запросов, суммарно %u слов",
            (unsigned)g_config.registers.size(), (unsigned)s_groups.size(), (unsigned)totalWords);
}

static bool ensureBound() {
  Stream *st = modbusTransportBegin();
  if (st == nullptr) {
    s_transportOk = false;
    return false;
  }
  s_transportOk = true;

  static Stream *boundStream = nullptr;
  if (st != boundStream) {
    node.begin(g_config.slave_id, *st);
    boundStream = st;
    logPrintf("[modbus] bound to transport, slave id=%u", g_config.slave_id);
  }
  if (s_boundRegisterCount != g_config.registers.size()) {
    rebuildGroups();
    s_boundRegisterCount = g_config.registers.size();
  }
  return true;
}

static void readGroup(const ReadGroup &g) {
  uint8_t result = node.readHoldingRegisters(g.startAddr, g.totalCount);
  if (result != node.ku8MBSuccess) {
    s_lastReadOk = false;
    logPrintf("[modbus] read error 0x%02X for group addr=%u count=%u (%u регистров)",
              result, g.startAddr, g.totalCount, (unsigned)g.members.size());
    return;
  }

  s_lastReadOk = true;
  for (uint16_t idx : g.members) {
    const RegisterDef &r = g_config.registers[idx];
    uint16_t offset = r.address - g.startAddr; // смещение внутри полученного блока, в словах

    int32_t raw;
    if (r.count >= 2) {
      raw = ((int32_t)node.getResponseBuffer(offset) << 16) | node.getResponseBuffer(offset + 1);
    } else {
      uint16_t raw16 = node.getResponseBuffer(offset);
      raw = r.is_signed ? (int16_t)raw16 : (int32_t)raw16;
    }
    float value = (float)raw * r.scale;

    ModbusResult res{idx, value};
    xQueueSend(s_resultQueue, &res, 0); // не блокируем задачу, если очередь вдруг полна
  }
}

static void drainWriteQueue() {
  if (!s_writeQueue) return;
  WriteRequest wr;
  while (xQueueReceive(s_writeQueue, &wr, 0) == pdTRUE) {
    uint8_t res = node.writeSingleRegister(wr.address, wr.rawValue);
    bool ok = (res == node.ku8MBSuccess);
    if (ok) {
      logPrintf("[modbus] запись addr=%u value=%u -- OK", wr.address, wr.rawValue);
    } else {
      logPrintf("[modbus] запись addr=%u value=%u -- ОШИБКА 0x%02X", wr.address, wr.rawValue, res);
    }
    if (wr.resultQueue) {
      ModbusWriteResult result{ok, res};
      xQueueSend(wr.resultQueue, &result, 0);
    }
  }
}

static void idleWait(uint32_t ms) {
  // Ждём указанное время НЕ одним долгим vTaskDelay, а мелкими кусками --
  // чтобы запрос на запись регистра (см. drainWriteQueue()) выполнялся
  // с задержкой не больше ~200мс, а не ждал до конца текущей паузы между
  // проходами опроса (которая может быть 15+ секунд).
  const uint32_t STEP_MS = 200;
  uint32_t waited = 0;
  while (waited < ms) {
    drainWriteQueue();
    uint32_t step = (ms - waited < STEP_MS) ? (ms - waited) : STEP_MS;
    vTaskDelay(pdMS_TO_TICKS(step));
    waited += step;
  }
}

static void modbusTaskFn(void *) {
  logPrintln("[modbus] задача опроса запущена (ядро 0)");
  for (;;) {
    if (!ensureBound() || g_config.registers.empty()) {
      idleWait(g_config.poll_interval_ms);
      continue;
    }

    for (const auto &g : s_groups) {
      readGroup(g);
      drainWriteQueue(); // не заставляем запись ждать до конца прохода
      vTaskDelay(pdMS_TO_TICKS(30)); // бережнее к RS485-шине между запросами
    }

    idleWait(g_config.poll_interval_ms);
  }
}

void modbusTaskStart() {
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  s_resultQueue = xQueueCreate(32, sizeof(ModbusResult));
  s_writeQueue = xQueueCreate(4, sizeof(WriteRequest));

  xTaskCreatePinnedToCore(modbusTaskFn, "modbus_poll", 4096, nullptr, 1, &s_taskHandle, 0);

  logPrintln("[modbus] задача опроса создана (ядро 0, параллельно веб-серверу на ядре 1)");
}

void modbusPollLoop(const RegisterValueCallback &onValue) {
  if (!s_resultQueue) return;
  ModbusResult res;
  while (xQueueReceive(s_resultQueue, &res, 0) == pdTRUE) {
    if (res.regIndex < g_config.registers.size()) {
      const RegisterDef &r = g_config.registers[res.regIndex];
      onValue(r.name, res.value, r.unit);
    }
  }
}

bool modbusIsHealthy() {
  return s_transportOk && s_lastReadOk;
}

void modbusTaskPause() {
  if (s_taskHandle) vTaskSuspend(s_taskHandle);
}

void modbusTaskResume() {
  if (s_taskHandle) vTaskResume(s_taskHandle);
}

ModbusWriteResult modbusWriteRegisterSync(uint16_t address, uint16_t rawValue, uint32_t timeoutMs) {
  ModbusWriteResult failResult{false, 0xFF}; // 0xFF -- условный код "не удалось даже поставить в очередь/дождаться"

  if (!s_writeQueue) return failResult;

  QueueHandle_t resultQueue = xQueueCreate(1, sizeof(ModbusWriteResult));
  if (!resultQueue) return failResult;

  WriteRequest req{address, rawValue, resultQueue};
  if (xQueueSend(s_writeQueue, &req, 0) != pdTRUE) {
    vQueueDelete(resultQueue);
    return failResult; // очередь записи переполнена (маловероятно, глубина 4)
  }

  ModbusWriteResult result;
  bool got = xQueueReceive(resultQueue, &result, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  vQueueDelete(resultQueue);

  if (!got) {
    logPrintf("[modbus] запись addr=%u -- не дождались результата за %u мс", address, (unsigned)timeoutMs);
    return failResult;
  }
  return result;
}
