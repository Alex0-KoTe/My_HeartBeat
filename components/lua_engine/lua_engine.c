#include "lua_engine.h"
#include "mcp23017.h"
#include "task_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "LUA_ENGINE";

// Глобальный Lua-стейт
static lua_State *L = NULL;

// ============================================================================
// Кастомная функция print (перехватывает вывод в терминал + UART)
// ============================================================================
static int l_custom_print(lua_State *L) {
    int n = lua_gettop(L);
    char buffer[512];
    int offset = 0;
    
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\t");
        }
        
        if (lua_isstring(L, i)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s", lua_tostring(L, i));
        } else if (lua_isboolean(L, i)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s", 
                              lua_toboolean(L, i) ? "true" : "false");
        } else if (lua_isnil(L, i)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "nil");
        } else if (lua_isnumber(L, i)) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%g", lua_tonumber(L, i));
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s: %p", 
                              luaL_typename(L, i), lua_topointer(L, i));
        }
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n");
    
    // Отправляем в лог Task Manager (для веб-терминала)
    task_manager_log("%s", buffer);
    
    // Дублируем в UART (монитор порта)
    printf("%s", buffer);
    
    return 0;
}

// ============================================================================
// Функция задержки для Lua: delay(мс)
// ============================================================================
static int l_delay(lua_State *L) {
    int ms = luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

// ============================================================================
// Инициализация Lua-движка
// ============================================================================
esp_err_t lua_engine_init(void) {
    L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return ESP_FAIL;
    }
    
    luaL_openlibs(L);

    // 1. Переопределяем print для перехвата вывода
    lua_pushcfunction(L, l_custom_print);
    lua_setglobal(L, "print");

    // 2. Регистрируем функцию задержки
    lua_pushcfunction(L, l_delay);
    lua_setglobal(L, "delay");

    // 3. Регистрируем биндинги MCP23017 (реле, входы)
    mcp23017_register_lua(L);
    ESP_LOGI(TAG, "MCP23017 Lua bindings registered");

    // 4. Регистрируем биндинги Task Manager (wdt_feed)
    task_manager_register_lua_bindings(L); 
    ESP_LOGI(TAG, "Task Manager Lua bindings registered");

    ESP_LOGI(TAG, "Lua engine fully initialized");
    return ESP_OK;
}

// ============================================================================
// Выполнение Lua-файла
// ============================================================================
esp_err_t lua_engine_execute_file(const char* path) {
    if (!L) {
        ESP_LOGE(TAG, "Lua engine not initialized");
        return ESP_FAIL;
    }
    
    // Формируем полный путь в SPIFFS
    char full_path[256];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "/spiffs%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "/spiffs/%s", path);
    }
    
    ESP_LOGI(TAG, "Executing Lua file: %s", full_path);
    
    int result = luaL_dofile(L, full_path);
    
    if (result != LUA_OK) {
        const char* err_msg = lua_tostring(L, -1);
        ESP_LOGE(TAG, "Lua error: %s", err_msg ? err_msg : "unknown error");
        task_manager_log("[LUA ERROR] %s\n", err_msg ? err_msg : "unknown error");
        lua_pop(L, 1); // Убираем ошибку из стека
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Executed Lua file: %s", full_path);
    return ESP_OK;
}