#ifndef MCP23017_CONFIG_H
#define MCP23017_CONFIG_H

// Настройки дебаунса
#define MCP23017_DEBOUNCE_ENABLED       1       // 1 = включен, 0 = выключен
#define MCP23017_DEBOUNCE_CYCLES        3       // количество стабильных циклов опроса

// Интервал опроса для MCP без прерываний (мс)
#define MCP23017_POLL_INTERVAL_MS       100

// Максимальное количество устройств на шине
#define MCP23017_MAX_DEVICES            8

// Порог ошибок перед блокировкой устройства
#define MCP23017_MAX_FAIL_COUNT         2

// Пины ESP32
#define MCP23017_SDA_PIN                GPIO_NUM_21
#define MCP23017_SCL_PIN                GPIO_NUM_22
#define MCP23017_INTB_PIN               GPIO_NUM_19
#define MCP23017_RESET_PIN              GPIO_NUM_4

// Адрес первого устройства с прерываниями
#define MCP23017_INTERRUPT_ADDRESS      0x20

#endif