#ifndef LUA_ENGINE_H
#define LUA_ENGINE_H

#include "esp_err.h"

esp_err_t lua_engine_init(void);
esp_err_t lua_engine_execute_file(const char* path);
esp_err_t lua_engine_execute_string(const char* code);

#endif