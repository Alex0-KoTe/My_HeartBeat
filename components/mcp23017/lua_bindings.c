#include "mcp23017.h"
#include "mcp23017_config.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>

// Объявляем внешние глобальные переменные, которые обновляются в mcp23017.c
extern uint8_t g_mcp_fault_flags;
extern uint8_t g_mcp_relay_states[MCP23017_MAX_DEVICES];

// ============================================================================
// Универсальные C-функции для Lua (используют upvalues для определения чипа/пина)
// ============================================================================

// Чтение одного входа (INx)
static int lua_mcp_in_read(lua_State *L) {
    int chip_idx = lua_tointeger(L, lua_upvalueindex(1));
    int pin = lua_tointeger(L, lua_upvalueindex(2));
    uint8_t address = 0x20 + chip_idx;
    
    bool value = mcp23017_read_pin_b(address, pin);
    lua_pushboolean(L, value);
    return 1;
}

// Чтение или запись одного реле (RLx)
static int lua_mcp_rl_rw(lua_State *L) {
    int chip_idx = lua_tointeger(L, lua_upvalueindex(1));
    int pin = lua_tointeger(L, lua_upvalueindex(2));
    uint8_t address = 0x20 + chip_idx;

    if (lua_gettop(L) == 0) {
        // Если аргументов нет -> это ЧТЕНИЕ
        uint8_t state = mcp23017_get_relay_state(address);
        lua_pushboolean(L, (state >> pin) & 1);
        return 1;
    } else {
        // Если есть аргумент -> это ЗАПИСЬ
        bool value = lua_toboolean(L, 1);
        mcp23017_write_pin_a(address, pin, value);
        return 0;
    }
}

// Чтение всего порта входов (IN_ALL)
static int lua_mcp_in_all_rw(lua_State *L) {
    int chip_idx = lua_tointeger(L, lua_upvalueindex(1));
    uint8_t address = 0x20 + chip_idx;
    uint8_t value;
    
    if (mcp23017_read_port_b(address, &value) == ESP_OK) {
        lua_pushinteger(L, value);
    } else {
        lua_pushnil(L); // Возвращаем nil при ошибке I2C
    }
    return 1;
}

// Чтение или запись всего порта реле (RL_ALL)
static int lua_mcp_rl_all_rw(lua_State *L) {
    int chip_idx = lua_tointeger(L, lua_upvalueindex(1));
    uint8_t address = 0x20 + chip_idx;

    if (lua_gettop(L) == 0) {
        // ЧТЕНИЕ
        lua_pushinteger(L, mcp23017_get_relay_state(address));
        return 1;
    } else {
        // ЗАПИСЬ
        uint8_t value = (uint8_t)lua_tointeger(L, 1);
        mcp23017_write_port_a(address, value);
        return 0;
    }
}

// Функция синхронизации глобальных переменных C -> Lua
static int lua_update_mcp_globals(lua_State *L) {
    // 1. Обновляем битовую маску аварий
    lua_pushinteger(L, g_mcp_fault_flags);
    lua_setglobal(L, "MCP_FAULT_FLAGS");
    
    // 2. Обновляем состояния реле для всех возможных 8 микросхем
    for (int i = 0; i < MCP23017_MAX_DEVICES; i++) {
        char var_name[32];
        snprintf(var_name, sizeof(var_name), "RELAY_STATE_%d", i);
        lua_pushinteger(L, g_mcp_relay_states[i]);
        lua_setglobal(L, var_name);
    }
    
    return 0; // Функция ничего не возвращает в стек
}

// ============================================================================
// Главная функция регистрации всех Lua-привязок
// ============================================================================
void mcp23017_register_lua(void *L_ptr) {
    lua_State *L = (lua_State *)L_ptr;
    char func_name[32];

    // Проходим по всем возможным 8 микросхемам (индекс 0 = адрес 0x20, индекс 1 = 0x21 и т.д.)
    for (int chip_idx = 0; chip_idx < MCP23017_MAX_DEVICES; chip_idx++) {
        
        // 1. Регистрируем функции чтения входов (IN0..IN7, 1IN0..1IN7, и т.д.)
        for (int pin = 0; pin < 8; pin++) {
            if (chip_idx == 0) {
                snprintf(func_name, sizeof(func_name), "IN%d", pin);
            } else {
                snprintf(func_name, sizeof(func_name), "%dIN%d", chip_idx, pin);
            }
            
            // Кладем в стек upvalue: сначала chip_idx, потом pin
            lua_pushinteger(L, chip_idx);
            lua_pushinteger(L, pin);
            // Создаем замыкание, которое "запоминает" эти 2 значения
            lua_pushcclosure(L, lua_mcp_in_read, 2);
            lua_setglobal(L, func_name);
        }

        // 2. Регистрируем функции управления реле (RL0..RL7, 1RL0..1RL7, и т.д.)
        for (int pin = 0; pin < 8; pin++) {
            if (chip_idx == 0) {
                snprintf(func_name, sizeof(func_name), "RL%d", pin);
            } else {
                snprintf(func_name, sizeof(func_name), "%dRL%d", chip_idx, pin);
            }
            
            lua_pushinteger(L, chip_idx);
            lua_pushinteger(L, pin);
            lua_pushcclosure(L, lua_mcp_rl_rw, 2);
            lua_setglobal(L, func_name);
        }

        // 3. Регистрируем функции для всего порта входов (IN_ALL, 1IN_ALL, и т.д.)
        if (chip_idx == 0) {
            snprintf(func_name, sizeof(func_name), "IN_ALL");
        } else {
            snprintf(func_name, sizeof(func_name), "%dIN_ALL", chip_idx);
        }
        lua_pushinteger(L, chip_idx);
        lua_pushcclosure(L, lua_mcp_in_all_rw, 1); // 1 upvalue (только chip_idx)
        lua_setglobal(L, func_name);

        // 4. Регистрируем функции для всего порта реле (RL_ALL, 1RL_ALL, и т.д.)
        if (chip_idx == 0) {
            snprintf(func_name, sizeof(func_name), "RL_ALL");
        } else {
            snprintf(func_name, sizeof(func_name), "%dRL_ALL", chip_idx);
        }
        lua_pushinteger(L, chip_idx);
        lua_pushcclosure(L, lua_mcp_rl_all_rw, 1); // 1 upvalue (только chip_idx)
        lua_setglobal(L, func_name);
    }

    // 5. Регистрируем функцию обновления глобальных переменных для отладчика
    lua_pushcfunction(L, lua_update_mcp_globals);
    lua_setglobal(L, "UPDATE_MCP_GLOBALS");
}