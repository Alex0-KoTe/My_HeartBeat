#include "mcp23017.h"
#include "mcp23017_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "MCP23017";

// ============================================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (доступны для lua_bindings.c через extern)
// ============================================================================
uint8_t g_mcp_fault_flags = 0;
uint8_t g_mcp_relay_states[MCP23017_MAX_DEVICES] = {0};

// Внутреннее состояние
static mcp23017_device_t devices[MCP23017_MAX_DEVICES];
static int device_count = 0;

// ============================================================================
// Внутренние функции I2C
// ============================================================================
static esp_err_t mcp23017_write_reg(uint8_t address, uint8_t reg, uint8_t value) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t mcp23017_read_reg(uint8_t address, uint8_t reg, uint8_t *value) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // Repeated start
    i2c_master_write_byte(cmd, (address << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, value, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ============================================================================
// Инициализация и управление устройствами
// ============================================================================
static esp_err_t mcp23017_init_device(mcp23017_device_t *dev) {
    // Порт A: все выходы (0x00), Порт B: все входы (0xFF)
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_IODIRA, 0x00));
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_IODIRB, 0xFF));
    
    // Полярность: прямая (0x00), так как оптроны активны при HIGH
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_IPOLA, 0x00));
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_IPOLB, 0x00));
    
    // Подтягивающие резисторы на порту B: включены (0xFF)
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_GPPUB, 0xFF));
    
    // Начальное состояние реле: все выключены (0x00)
    ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_OLATA, 0x00));
    dev->last_port_a = 0x00;
    g_mcp_relay_states[dev->address - 0x20] = 0x00;
    
    // Читаем начальное значение порта B
    uint8_t initial_b;
    ESP_ERROR_CHECK(mcp23017_read_reg(dev->address, MCP23017_GPIOB, &initial_b));
    dev->last_port_b = initial_b;
    dev->defval_b = initial_b;
    
    // Сброс счетчиков дебаунса
    memset(dev->debounce_counter, 0, sizeof(dev->debounce_counter));
    memset(dev->debounce_shadow, 0, sizeof(dev->debounce_shadow));

    // Настройка прерываний (только если запрошено)
    if (dev->use_interrupts) {
        ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_GPINTENB, 0xFF)); // Разрешить на всех пинах
        ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_INTCONB, 0xFF));  // Режим сравнения с DEFVAL
        ESP_ERROR_CHECK(mcp23017_write_reg(dev->address, MCP23017_DEFVALB, dev->defval_b));
    }
    
    dev->active = true;
    dev->fail_count = 0;
    
    // Снимаем флаг аварии для этого устройства
    g_mcp_fault_flags &= ~(1 << (dev->address - 0x20));
    
    ESP_LOGI(TAG, "Device 0x%02X initialized", dev->address);
    return ESP_OK;
}

static esp_err_t mcp23017_reinit_device(mcp23017_device_t *dev) {
    ESP_LOGW(TAG, "Attempting to reinitialize device 0x%02X", dev->address);
    dev->active = false; // Временно помечаем как неактивное
    esp_err_t ret = mcp23017_init_device(dev);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Device 0x%02X reinitialized successfully", dev->address);
    } else {
        ESP_LOGE(TAG, "Failed to reinitialize device 0x%02X", dev->address);
    }
    return ret;
}

// ============================================================================
// Публичный API
// ============================================================================
esp_err_t mcp23017_bus_init(gpio_num_t sda, gpio_num_t scl, uint32_t freq_hz) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq_hz,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));
    
    // Сброс MCP23017 (RESET активный HIGH)
    gpio_reset_pin(MCP23017_RESET_PIN);
    gpio_set_direction(MCP23017_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(MCP23017_RESET_PIN, 1);  // HIGH = чип работает
    vTaskDelay(pdMS_TO_TICKS(10));          // Ждем стабилизации
    
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, freq=%d Hz)", sda, scl, freq_hz);
    return ESP_OK;
}

esp_err_t mcp23017_add_device(uint8_t address, bool use_interrupts) {
    if (device_count >= MCP23017_MAX_DEVICES) {
        ESP_LOGE(TAG, "Maximum devices (%d) reached", MCP23017_MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }
    
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES) {
        ESP_LOGE(TAG, "Invalid address 0x%02X (must be 0x20-0x27)", address);
        return ESP_ERR_INVALID_ARG;
    }
    
    devices[idx].address = address;
    devices[idx].use_interrupts = use_interrupts;
    device_count++;
    
    return mcp23017_init_device(&devices[idx]);
}

esp_err_t mcp23017_read_port_b(uint8_t address, uint8_t *value) {
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES || !devices[idx].active) {
        return ESP_ERR_INVALID_STATE;
    }
    return mcp23017_read_reg(address, MCP23017_GPIOB, value);
}

esp_err_t mcp23017_write_port_a(uint8_t address, uint8_t value) {
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES || !devices[idx].active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = mcp23017_write_reg(address, MCP23017_OLATA, value);
    if (ret == ESP_OK) {
        devices[idx].last_port_a = value;
        g_mcp_relay_states[idx] = value; // Обновляем глобальную переменную для Lua
    }
    return ret;
}

bool mcp23017_read_pin_b(uint8_t address, uint8_t pin) {
    uint8_t value;
    if (mcp23017_read_port_b(address, &value) != ESP_OK) {
        return false;
    }
    return (value >> pin) & 1;
}

esp_err_t mcp23017_write_pin_a(uint8_t address, uint8_t pin, bool value) {
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES || !devices[idx].active) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t new_value = devices[idx].last_port_a;
    if (value) {
        new_value |= (1 << pin);
    } else {
        new_value &= ~(1 << pin);
    }
    
    esp_err_t ret = mcp23017_write_reg(address, MCP23017_OLATA, new_value);
    if (ret == ESP_OK) {
        devices[idx].last_port_a = new_value;
        g_mcp_relay_states[idx] = new_value; // Обновляем глобальную переменную для Lua
    }
    return ret;
}

uint8_t mcp23017_get_relay_state(uint8_t address) {
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES) {
        return 0;
    }
    return g_mcp_relay_states[idx];
}

bool mcp23017_get_fault(uint8_t address) {
    int idx = address - 0x20;
    if (idx < 0 || idx >= MCP23017_MAX_DEVICES) {
        return true; // Считаем несуществующий адрес как "сбойный"
    }
    return (g_mcp_fault_flags & (1 << idx)) != 0;
}

// ============================================================================
// Фоновая задача (Опрос и обработка прерываний)
// ============================================================================
static void mcp23017_task(void *pvParameters) {
    // Настройка GPIO для прерывания (INTB активный LOW, поэтому NEGEDGE)
    bool has_interrupts = false;
    for (int i = 0; i < MCP23017_MAX_DEVICES; i++) {
        if (devices[i].active && devices[i].use_interrupts) {
            has_interrupts = true;
            break;
        }
    }

    if (has_interrupts) {
        gpio_reset_pin(MCP23017_INTB_PIN);
        gpio_set_direction(MCP23017_INTB_PIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode(MCP23017_INTB_PIN, GPIO_PULLUP_ONLY);
        gpio_set_intr_type(MCP23017_INTB_PIN, GPIO_INTR_NEGEDGE);
        ESP_LOGI(TAG, "INTB configured on GPIO %d", MCP23017_INTB_PIN);
    }
    
    while (1) {
        for (int i = 0; i < MCP23017_MAX_DEVICES; i++) {
            if (!devices[i].active) continue;

            uint8_t port_b;
            esp_err_t ret = mcp23017_read_reg(devices[i].address, MCP23017_GPIOB, &port_b);
            
            if (ret == ESP_OK) {
                devices[i].fail_count = 0;
                g_mcp_fault_flags &= ~(1 << i); // Сбрасываем флаг аварии

                if (devices[i].use_interrupts) {
                    // Логика прерываний: сработало, если уровень INTB низкий
                    if (gpio_get_level(MCP23017_INTB_PIN) == 0) {
                        if (port_b != devices[i].defval_b) {
                            // Сигнал изменился, обновляем опорное значение
                            devices[i].defval_b = port_b;
                            devices[i].last_port_b = port_b;
                            mcp23017_write_reg(devices[i].address, MCP23017_DEFVALB, port_b);
                            ESP_LOGD(TAG, "Device 0x%02X: Port B changed to 0x%02X", 
                                     devices[i].address, port_b);
                        }
                    }
                } else {
                    // Логика опроса с дебаунсом (для чипов 0x21 и далее)
                    #if MCP23017_DEBOUNCE_ENABLED
                    for (int pin = 0; pin < 8; pin++) {
                        bool current_bit = (port_b >> pin) & 1;
                        
                        if (current_bit == devices[i].debounce_shadow[pin]) {
                            if (devices[i].debounce_counter[pin] < MCP23017_DEBOUNCE_CYCLES) {
                                devices[i].debounce_counter[pin]++;
                            }
                            // Если достигли порога стабильности, применяем новое значение
                            if (devices[i].debounce_counter[pin] >= MCP23017_DEBOUNCE_CYCLES) {
                                bool last_bit = (devices[i].last_port_b >> pin) & 1;
                                if (current_bit != last_bit) {
                                    if (current_bit) {
                                        devices[i].last_port_b |= (1 << pin);
                                    } else {
                                        devices[i].last_port_b &= ~(1 << pin);
                                    }
                                }
                            }
                        } else {
                            // Значение изменилось, сбрасываем счетчик и запоминаем новое "теневое" значение
                            devices[i].debounce_shadow[pin] = current_bit;
                            devices[i].debounce_counter[pin] = 0;
                        }
                    }
                    #else
                    devices[i].last_port_b = port_b;
                    #endif
                }
            } else {
                // Обработка ошибки I2C
                devices[i].fail_count++;
                if (devices[i].fail_count >= MCP23017_MAX_FAIL_COUNT) {
                    devices[i].active = false;
                    g_mcp_fault_flags |= (1 << i); // Устанавливаем флаг аварии
                    ESP_LOGE(TAG, "Device 0x%02X fault after %d failures", 
                             devices[i].address, devices[i].fail_count);
                } else {
                    mcp23017_reinit_device(&devices[i]);
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(MCP23017_POLL_INTERVAL_MS));
    }
}

esp_err_t mcp23017_start_task(void) {
    xTaskCreate(mcp23017_task, "mcp23017_task", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "MCP23017 background task started");
    return ESP_OK;
}