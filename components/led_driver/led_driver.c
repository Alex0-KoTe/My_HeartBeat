#include "led_driver.h"
#include "esp_log.h"

static const char* TAG = "LED_DRIVER";

esp_err_t led_init(led_config_t* config) {
    gpio_reset_pin(config->pin);
    gpio_set_direction(config->pin, GPIO_MODE_OUTPUT);
    config->state = false;
    
    ESP_LOGI(TAG, "LED initialized on GPIO %d", config->pin);
    return ESP_OK;
}

esp_err_t led_on(led_config_t* config) {
    gpio_set_level(config->pin, 1);
    config->state = true;
    return ESP_OK;
}

esp_err_t led_off(led_config_t* config) {
    gpio_set_level(config->pin, 0);
    config->state = false;
    return ESP_OK;
}

esp_err_t led_toggle(led_config_t* config) {
    config->state = !config->state;
    gpio_set_level(config->pin, config->state ? 1 : 0);
    return ESP_OK;
}