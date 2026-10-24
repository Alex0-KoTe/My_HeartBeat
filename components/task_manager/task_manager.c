#include "task_manager.h"
#include "lua_engine.h"
#include "lua.h"
#include "lauxlib.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *TAG = "TASK_MGR";

// ============================================================================
// Кольцевой буфер логов (для веб-терминала)
// ============================================================================
#define LOG_BUFFER_SIZE 4096
static char log_buffer[LOG_BUFFER_SIZE];
static int log_write_pos = 0;
static SemaphoreHandle_t log_mutex = NULL;

void task_manager_log(const char* fmt, ...) {
    if (log_mutex == NULL) return;
    
    char temp[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);
    
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    // Копируем в буфер, не выходя за границы
    for (int i = 0; i < len && log_write_pos < LOG_BUFFER_SIZE - 1; i++) {
        log_buffer[log_write_pos++] = temp[i];
    }
    log_buffer[log_write_pos] = '\0';
    
    // Если буфер заполняется, сдвигаем содержимое
    if (log_write_pos >= LOG_BUFFER_SIZE - 200) {
        memmove(log_buffer, log_buffer + 1000, LOG_BUFFER_SIZE - 1000);
        log_write_pos -= 1000;
    }
    
    xSemaphoreGive(log_mutex);
}

const char* task_manager_get_logs(void) {
    return log_buffer;
}

void task_manager_clear_logs(void) {
    if (log_mutex == NULL) return;
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_write_pos = 0;
    log_buffer[0] = '\0';
    xSemaphoreGive(log_mutex);
}

// ============================================================================
// Внутреннее состояние менеджера
// ============================================================================
static SemaphoreHandle_t mgr_mutex = NULL;
static TaskHandle_t current_task_handle = NULL;
static task_mgr_state_t current_state = TASK_MGR_STATE_IDLE;
static uint32_t last_wdt_feed_tick = 0;
static uint32_t current_wdt_timeout_ms = 0;

typedef struct {
    char script_path[256];
    uint32_t wdt_timeout_ms;
} task_params_t;

// ============================================================================
// Задача выполнения Lua-скрипта
// ============================================================================
static void managed_task_entry(void *pvParameters) {
    task_params_t *params = (task_params_t *)pvParameters;
    
    task_manager_log("[TASK] Started: %s\n", params->script_path);
    ESP_LOGI(TAG, "Task started: %s", params->script_path);
    
    last_wdt_feed_tick = xTaskGetTickCount();
    current_wdt_timeout_ms = params->wdt_timeout_ms;

    lua_engine_execute_file(params->script_path);

    task_manager_log("[TASK] Completed: %s\n", params->script_path);
    ESP_LOGI(TAG, "Task completed normally: %s", params->script_path);

    free(params);
    
    xSemaphoreTake(mgr_mutex, portMAX_DELAY);
    current_state = TASK_MGR_STATE_IDLE;
    current_task_handle = NULL;
    current_wdt_timeout_ms = 0;
    xSemaphoreGive(mgr_mutex);

    vTaskDelete(NULL);
}

// ============================================================================
// Задача-сторож (Watchdog Monitor)
// ============================================================================
static void wdt_monitor_task(void *pvParameters) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        
        xSemaphoreTake(mgr_mutex, portMAX_DELAY);
        
        if (current_state == TASK_MGR_STATE_RUNNING && current_wdt_timeout_ms > 0) {
            TickType_t current_tick = xTaskGetTickCount();
            uint32_t elapsed_ms = (current_tick - last_wdt_feed_tick) * portTICK_PERIOD_MS;
            
            if (elapsed_ms > current_wdt_timeout_ms) {
                task_manager_log("[WDT] TIMEOUT! Task exceeded %d ms without feeding.\n", 
                                 current_wdt_timeout_ms);
                ESP_LOGE(TAG, "WATCHDOG TIMEOUT! Task exceeded %d ms without feeding.", 
                         current_wdt_timeout_ms);
                current_state = TASK_MGR_STATE_WATCHDOG_TIMEOUT;
                
                if (current_task_handle != NULL) {
                    vTaskDelete(current_task_handle);
                    current_task_handle = NULL;
                }
                current_wdt_timeout_ms = 0;
            }
        }
        
        xSemaphoreGive(mgr_mutex);
    }
}

// ============================================================================
// Публичный API
// ============================================================================
esp_err_t task_manager_init(void) {
    if (mgr_mutex == NULL) {
        mgr_mutex = xSemaphoreCreateMutex();
        if (mgr_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
        if (log_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    
    log_buffer[0] = '\0';
    
    xTaskCreate(wdt_monitor_task, "wdt_mon", 2048, NULL, 3, NULL);
    
    task_manager_log("[MGR] Task Manager initialized\n");
    ESP_LOGI(TAG, "Task Manager initialized");
    return ESP_OK;
}

esp_err_t task_manager_run_lua(const char* script_path, const task_mgr_config_t* config) {
    if (config == NULL || script_path == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(mgr_mutex, portMAX_DELAY);
    
    if (current_state == TASK_MGR_STATE_RUNNING) {
        xSemaphoreGive(mgr_mutex);
        task_manager_log("[MGR] Rejected: Task already running\n");
        ESP_LOGW(TAG, "Rejected: Task already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t required_heap = config->stack_size + 20480;
    
    if (free_heap < required_heap) {
        current_state = TASK_MGR_STATE_MEMORY_ERROR;
        xSemaphoreGive(mgr_mutex);
        task_manager_log("[MGR] Rejected: Not enough memory. Free: %lu, Required: %lu\n", 
                         free_heap, required_heap);
        ESP_LOGE(TAG, "Rejected: Not enough memory. Free: %lu, Required: %lu", 
                 free_heap, required_heap);
        return ESP_ERR_NO_MEM;
    }
    
    task_params_t *params = malloc(sizeof(task_params_t));
    if (params == NULL) {
        xSemaphoreGive(mgr_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    strncpy(params->script_path, script_path, sizeof(params->script_path) - 1);
    params->script_path[sizeof(params->script_path) - 1] = '\0';
    params->wdt_timeout_ms = config->wdt_timeout_ms;
    
    BaseType_t ret = xTaskCreate(
        managed_task_entry,
        config->task_name ? config->task_name : "lua_task",
        config->stack_size,
        params,
        5,
        &current_task_handle
    );
    
    if (ret != pdPASS) {
        free(params);
        current_state = TASK_MGR_STATE_CRASHED;
        xSemaphoreGive(mgr_mutex);
        task_manager_log("[MGR] Failed to create FreeRTOS task\n");
        ESP_LOGE(TAG, "Failed to create FreeRTOS task");
        return ESP_FAIL;
    }
    
    current_state = TASK_MGR_STATE_RUNNING;
    xSemaphoreGive(mgr_mutex);
    
    task_manager_log("[MGR] Task started successfully\n");
    ESP_LOGI(TAG, "Task started successfully");
    return ESP_OK;
}

esp_err_t task_manager_stop_current(void) {
    xSemaphoreTake(mgr_mutex, portMAX_DELAY);
    if (current_state == TASK_MGR_STATE_RUNNING && current_task_handle != NULL) {
        vTaskDelete(current_task_handle);
        current_task_handle = NULL;
        current_state = TASK_MGR_STATE_IDLE;
        current_wdt_timeout_ms = 0;
        xSemaphoreGive(mgr_mutex);
        task_manager_log("[MGR] Task forcefully stopped\n");
        ESP_LOGI(TAG, "Task forcefully stopped");
        return ESP_OK;
    }
    xSemaphoreGive(mgr_mutex);
    return ESP_ERR_INVALID_STATE;
}

task_mgr_state_t task_manager_get_state(void) {
    return current_state;
}

// ============================================================================
// Lua Bindings
// ============================================================================
static int l_wdt_feed(lua_State *L) {
    xSemaphoreTake(mgr_mutex, portMAX_DELAY);
    if (current_state == TASK_MGR_STATE_RUNNING) {
        last_wdt_feed_tick = xTaskGetTickCount();
    }
    xSemaphoreGive(mgr_mutex);
    return 0;
}

void task_manager_register_lua_bindings(void* L_ptr) {
    lua_State *L = (lua_State *)L_ptr;
    
    lua_pushcfunction(L, l_wdt_feed);
    lua_setglobal(L, "wdt_feed");
    
    task_manager_log("[MGR] Lua bindings registered (wdt_feed)\n");
    ESP_LOGI(TAG, "Lua bindings registered (wdt_feed)");
}