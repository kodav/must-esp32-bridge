#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>

// Одна "виртуальная точка данных" -- аналог ESPHome modbus_controller sensor
struct RegisterDef {
  String name;      // используется в MQTT-топике и как unique_id -- технический, латиницей
  String name_ru;    // отображаемое имя в Home Assistant (поле "name" в discovery).
                      // Если пусто -- используется name как раньше.
  uint16_t address;
  uint8_t  count;    // сколько 16-битных регистров занимает значение (1 или 2)
  bool     is_signed; // трактовать как знаковое (int16/int32)
  float    scale;    // множитель: raw * scale = итоговое значение
  String   unit;     // "V", "A", "W", "%", "" и т.д.
  bool     writable; // true -- регистр является настраиваемым параметром
                      // (Platform.NUMBER в оригинальной карте ha-must-inverter),
                      // а не просто измерением -- только такие предлагаются
                      // для записи в веб-интерфейсе. НЕ давать значение по
                      // умолчанию здесь -- на некоторых толчейнах/стандартах
                      // C++ default member initializer ломает агрегатную
                      // инициализацию структуры целиком (см. историю фиксов).
                      // Каждая запись в config.cpp должна указывать это поле явно.

  // Диапазон для writable-регистров -- используется и веб-формой (в
  // будущем), и MQTT discovery как number-сущность (слайдер в HA).
  // Для НЕ writable регистров эти три поля просто не используются, можно
  // не указывать в JSON/initializer-list (аггрегатно инициализируются в 0).
  float min_value;
  float max_value;
  float step;

  // true -- регистр является булевым переключателем (0/1), а не числовым
  // параметром -- публикуется в HA как switch, а не number/slider.
  // Осмысленно только вместе с writable=true. БЕЗ значения по умолчанию
  // (та же причина, что и у writable/enum_map выше).
  bool is_switch;

  // true -- значение приходит НЕ по Modbus, а из другого источника
  // (например, локальный I2C-датчик температуры/влажности) -- задача
  // опроса Modbus пропускает такие регистры при построении групп чтения,
  // публикацию делает соответствующий модуль напрямую. address/count в
  // этом случае не используются (можно 0/1). БЕЗ значения по умолчанию
  // (та же причина, что и у writable/enum_map выше).
  bool is_local;

  // Необязательная расшифровка кодов enum-регистров (например WorkState:
  // 0="Включение", 4="Bypass" и т.д.) -- код->текст. Пусто для обычных
  // числовых регистров (подавляющее большинство). Если непусто -- в MQTT
  // публикуется текст вместо числа, а в HA discovery -- device_class:enum.
  // БЕЗ значения по умолчанию (см. комментарий про writable выше) --
  // но это ОК: как и с writable, отсутствующий 9й член при агрегатной
  // инициализации просто value-initialized (пустой vector), доп. правка
  // старых 63 записей не нужна -- только у новых регистров, где enum_map
  // реально указан.
  std::vector<std::pair<int, String>> enum_map;

  // Как enum_map, но для битовых масок ошибок/предупреждений: ключ --
  // номер БИТА (не значения), может быть установлено несколько бит
  // одновременно -- в MQTT публикуется текст через запятую со всеми
  // активными, либо "OK", если ничего не установлено (raw==0). БЕЗ
  // значения по умолчанию по той же причине, что enum_map/writable выше.
  std::vector<std::pair<int, String>> bit_map;
};

struct AppConfig {
  // ---- WiFi ----
  String wifi_ssid;
  String wifi_password;

  // ---- MQTT ----
  String mqtt_host = "";
  uint16_t mqtt_port = 1883;
  String mqtt_user = "";
  String mqtt_password = "";
  String mqtt_client_id = "must-pv18-esp32";
  String mqtt_topic_prefix = "must_pv18";
  bool mqtt_ha_discovery = true;
  bool mqtt_tls = false;               // включите, если брокер слушает TLS (обычно порт 8883)
                                        // ВАЖНО: используется setInsecure() -- шифрование канала
                                        // есть, но сертификат брокера не проверяется (см. README)

  // ---- Локальная история (для просмотра в вебе таблицей/графиком) ----
  // Копится в PSRAM независимо от состояния MQTT/WiFi -- см. history.cpp.
  bool history_enabled = true;
  float history_hours = 24.0f; // жёстко ограничено сверху 72ч в коде -- защита от гигантской аллокации

  // ---- OTA / веб-доступ ----
  String ota_password = "";           // если пусто -- без пароля (не рекомендуется в проде)

  // ---- RGB-индикатор состояния (WS2812/NeoPixel) ----
  bool rgb_led_enabled = true;
  int rgb_led_pin = 48;      // GPIO48 -- типичный пин на ESP32-S3-DevKitC-1,
                              // на вашем клоне может отличаться -- см. README
  int rgb_led_brightness = 40; // 0-255, по умолчанию неяркий -- это индикатор,
                                // а не фонарик

  // ---- Датчик температуры/влажности SHT41 (I2C, локальный) ----
  bool sht41_enabled = false; // выключен по умолчанию, пока физически не подключён
  int sht41_sda_pin = 8;      // типичные I2C-пины по умолчанию на ESP32-S3,
  int sht41_scl_pin = 9;      // на вашем клоне могут отличаться

  // ---- Доступ к веб-интерфейсу и API (HTTP Basic Auth) ----
  // Пусто по умолчанию -- защита выключена, чтобы не оказаться запертым
  // при первой настройке (AP-режим и так изолирован собственным WiFi-паролем).
  // Как только заполните пароль здесь и сохраните -- защита включится
  // на весь веб (кроме /update, у него отдельный ota_password выше).
  String web_username = "admin";
  String web_password = "";

  // ---- Modbus (RS485/UART) ----
  int uart_rx_pin = 16;
  int uart_tx_pin = 17;
  int uart_de_pin = -1;               // пин управления направлением RS485, -1 = не используется (авто-модуль)
  uint32_t uart_baud = 19200;
  uint8_t slave_id = 4;
  uint32_t poll_interval_ms = 15000;  // интервал МЕЖДУ полными проходами по всем
                                        // регистрам (не блокирует loop() -- опрос
                                        // неблокирующий, см. modbus_poll.cpp)

  std::vector<RegisterDef> registers;
};

extern AppConfig g_config;

bool configLoad();
bool configSave();
void configSetDefaults();

// Сериализация в/из JSON-строки -- используется и для файла, и для веб-API
String configToJson();
bool configFromJson(const String &json, String &errorOut);
