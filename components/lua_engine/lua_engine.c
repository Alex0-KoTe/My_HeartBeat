#include "lua_engine.h"
#include "spiffs_manager.h"
#include "esp_log.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <string.h>

static const char* TAG = "LUA_ENGINE";
static lua_State* L = NULL;

esp_err_t lua_engine_init(void) {
    L = luaL_newstate();
    if (!L) {
        ESP_LOGE(TAG, "Failed to create Lua state");
        return ESP_FAIL;
    }
    
    luaL_openlibs(L);
    ESP_LOGI(TAG, "Lua engine initialized");
    
    return ESP_OK;
}

esp_err_t lua_engine_execute_file(const char* path) {
    if (!L) {
        ESP_LOGE(TAG, "Lua engine not initialized");
        return ESP_FAIL;
    }
    
    char buffer[4096];
    esp_err_t ret = spiffs_read_file(path, buffer, sizeof(buffer));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read Lua file: %s", path);
        return ret;
    }
    
    int result = luaL_dostring(L, buffer);
    if (result != LUA_OK) {
        ESP_LOGE(TAG, "Lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Executed Lua file: %s", path);
    return ESP_OK;
}

esp_err_t lua_engine_execute_string(const char* code) {
    if (!L) {
        ESP_LOGE(TAG, "Lua engine not initialized");
        return ESP_FAIL;
    }
    
    int result = luaL_dostring(L, code);
    if (result != LUA_OK) {
        ESP_LOGE(TAG, "Lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}