#ifndef MCP23017_H
#define MCP23017_H

#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "mcp23017_config.h" 
#include <stdint.h>
#include <stdbool.h>

// Регистры MCP23017
#define MCP23017_IODIRA     0x00
#define MCP23017_IODIRB     0x01
#define MCP23017_IPOLA      0x02
#define MCP23017_IPOLB      0x03
#define MCP23017_GPINTENA   0x04
#define MCP23017_GPINTENB   0x05
#define MCP23017_DEFVALA    0x06
#define MCP23017_DEFVALB    0x07
#define MCP23017_INTCONA    0x08
#define MCP23017_INTCONB    0x09
#define MCP23017_GPPUA      0x0C
#define MCP23017_GPPUB      0x0D
#define MCP23017_GPIOA      0x12
#define MCP23017_GPIOB      0x13
#define MCP23017_OLATA      0x14
#define MCP23017_OLATB      0x15

// Структура устройства
typedef struct {
    uint8_t address;
    bool active;
    bool use_interrupts;
    uint8_t defval_b;
    uint8_t last_port_b;
    uint8_t last_port_a;
    uint8_t fail_count;
    uint8_t debounce_counter[8];  // для каждого пина порта B
    uint8_t debounce_shadow[8]; 
} mcp23017_device_t;

// Инициализация шины I2C
esp_err_t mcp23017_bus_init(gpio_num_t sda, gpio_num_t scl, uint32_t freq_hz);

// Добавление устройства
esp_err_t mcp23017_add_device(uint8_t address, bool use_interrupts);

// Чтение всего порта B
esp_err_t mcp23017_read_port_b(uint8_t address, uint8_t *value);

// Запись всего порта A
esp_err_t mcp23017_write_port_a(uint8_t address, uint8_t value);

// Чтение одного пина порта B
bool mcp23017_read_pin_b(uint8_t address, uint8_t pin);

// Запись одного пина порта A
esp_err_t mcp23017_write_pin_a(uint8_t address, uint8_t pin, bool value);

// Получение состояния реле (из кэша)
uint8_t mcp23017_get_relay_state(uint8_t address);

// Получение флага аварии
bool mcp23017_get_fault(uint8_t address);

// Запуск задачи опроса/прерываний
esp_err_t mcp23017_start_task(void);

// Регистрация Lua-функций
void mcp23017_register_lua(void *L);
// Глобальные переменные для интеграции с Lua-отладчиком
extern uint8_t g_mcp_fault_flags;
extern uint8_t g_mcp_relay_states[MCP23017_MAX_DEVICES];

#endif