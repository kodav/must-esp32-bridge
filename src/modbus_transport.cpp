#include "modbus_transport.h"
#include "config.h"
#include "log_buffer.h"
#include <HardwareSerial.h>

// ---------------------------------------------------------------------
// RS485 / UART -- единственный транспорт в проекте.
// ---------------------------------------------------------------------
static HardwareSerial RS485Serial(2); // используем UART2

static bool s_uart_started = false;
static bool s_de_pin_used = false;

Stream *modbusTransportBegin() {
  if (!s_uart_started) {
    RS485Serial.begin(g_config.uart_baud, SERIAL_8N1,
                       g_config.uart_rx_pin, g_config.uart_tx_pin);
    s_de_pin_used = (g_config.uart_de_pin >= 0);
    if (s_de_pin_used) {
      pinMode(g_config.uart_de_pin, OUTPUT);
      digitalWrite(g_config.uart_de_pin, LOW); // приём по умолчанию
    }
    s_uart_started = true;
    logPrintf("[modbus] UART/RS485 transport started: RX=%d TX=%d DE=%d baud=%u",
              g_config.uart_rx_pin, g_config.uart_tx_pin, g_config.uart_de_pin,
              g_config.uart_baud);
  }
  return &RS485Serial;
}

bool modbusTransportNeedsDirectionControl() {
  return s_de_pin_used;
}

void modbusTransportSetTx() {
  if (s_de_pin_used) digitalWrite(g_config.uart_de_pin, HIGH);
}

void modbusTransportSetRx() {
  if (s_de_pin_used) digitalWrite(g_config.uart_de_pin, LOW);
}
