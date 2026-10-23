#include "led_driver.h"
#include "esp_log.h"

static const char* TAG = "LED_DRIVER";

esp_err_t led_init(led_config_t* config) {
    gpio_reset_pin(config->pin);
    gpio_set_direction(config->pin, GPIO_MODE_OUTPUT);
    config->state = false;
    
    // Гарантированно выключаем светодиод при старте
    led_off(config);
    
    ESP_LOGI(TAG, "LED initialized on GPIO %d (active %s)", 
             config->pin, config->active_low ? "LOW" : "HIGH");
    return ESP_OK;
}

// Внутренняя функция для применения состояния с учетом active_low
static void apply_state(led_config_t* config, bool on) {
    int level = config->active_low ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level(config->pin, level);
    config->state = on;
}

esp_err_t led_on(led_config_t* config) {
    apply_state(config, true);
    return ESP_OK;
}

esp_err_t led_off(led_config_t* config) {
    apply_state(config, false);
    return ESP_OK;
}