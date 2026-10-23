#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include "esp_err.h"
#include "driver/gpio.h"

typedef struct {
    gpio_num_t pin;
    bool state;
} led_config_t;

esp_err_t led_init(led_config_t* config);
esp_err_t led_on(led_config_t* config);
esp_err_t led_off(led_config_t* config);
esp_err_t led_toggle(led_config_t* config);

#endif