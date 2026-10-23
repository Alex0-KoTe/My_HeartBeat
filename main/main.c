#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"           // <-- ДОБАВИТЬ
#include "spiffs_manager.h"
#include "web_server.h"
#include "lua_engine.h"
#include "led_driver.h"

static const char* TAG = "MAIN";

static led_config_t led_config = {
    .pin = GPIO_NUM_2
};

// ====== НАСТРОЙКИ СЕТИ ======
#define WIFI_SSID       "KoTe"
#define WIFI_PASSWORD   "DYNFF39666"
#define STATIC_IP       "192.168.0.100"
#define STATIC_GW       "192.168.0.1"
#define STATIC_NETMASK  "255.255.255.0"
// ============================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi started, connecting to %s...", WIFI_SSID);
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void set_static_ip(esp_netif_t* netif) {
    esp_netif_dhcpc_stop(netif);
    
    esp_netif_ip_info_t ip_info;
    ip_info.ip.addr = esp_ip4addr_aton(STATIC_IP);
    ip_info.gw.addr = esp_ip4addr_aton(STATIC_GW);
    ip_info.netmask.addr = esp_ip4addr_aton(STATIC_NETMASK);
    
    esp_netif_set_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "Static IP set: %s", STATIC_IP);
}

void led_task(void* pvParameters) {
    while (1) {
        led_toggle(&led_config);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void lua_task(void* pvParameters) {
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    lua_engine_execute_file("/scripts/main.lua");
    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting application");
    
    // ===== 0. ИНИЦИАЛИЗАЦИЯ NVS (КРИТИЧНО!) =====
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
    // ==============================================
    
    // 1. SPIFFS
    ret = spiffs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS");
        return;
    }
    
    // 2. LED
    led_init(&led_config);
    
    // 3. Lua
    lua_engine_init();
    
    // 4. Сетевой стек
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 5. Сетевой интерфейс Wi-Fi Station
    esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
    
    // 6. Статический IP
    set_static_ip(sta_netif);
    
    // 7. Инициализация Wi-Fi (теперь NVS уже готов)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "Wi-Fi initialization complete");
    
    // 8. Web-сервер
    ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }
    
    // 9. Задачи
    xTaskCreate(led_task, "LED_TASK", 4096, NULL, 5, NULL);
    xTaskCreate(lua_task, "LUA_TASK", 8192, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Application started. Web server at http://%s", STATIC_IP);
}