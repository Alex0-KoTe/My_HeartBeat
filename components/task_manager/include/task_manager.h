#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Состояния менеджера задач
typedef enum {
    TASK_MGR_STATE_IDLE,
    TASK_MGR_STATE_RUNNING,
    TASK_MGR_STATE_CRASHED,
    TASK_MGR_STATE_MEMORY_ERROR,
    TASK_MGR_STATE_WATCHDOG_TIMEOUT
} task_mgr_state_t;

// Конфигурация запускаемой задачи
typedef struct {
    const char* task_name;      // Имя для отладки
    uint32_t stack_size;        // Размер стека (байт)
    uint32_t wdt_timeout_ms;    // Таймаут watchdog (0 = отключен)
    bool auto_restart;          // Автоперезапуск при сбое
} task_mgr_config_t;

// Тип callback-функции для событий Task Manager
// message - текст события (лог или статус)
typedef void (*task_mgr_event_callback_t)(const char* message);

// Инициализация менеджера (вызывается один раз при старте)
esp_err_t task_manager_init(void);

// Запуск Lua-скрипта в управляемой задаче
esp_err_t task_manager_run_lua(const char* script_path, const task_mgr_config_t* config);

// Принудительная остановка текущей задачи
esp_err_t task_manager_stop_current(void);

// Получение текущего состояния
task_mgr_state_t task_manager_get_state(void);

// Регистрация Lua-биндингов (wdt_feed и др.)
void task_manager_register_lua_bindings(void* L);

// === ФУНКЦИИ ДЛЯ ЛОГИРОВАНИЯ ===
void task_manager_log(const char* fmt, ...);
const char* task_manager_get_logs(void);
void task_manager_clear_logs(void);

// === НОВОЕ: Регистрация callback для WebSocket ===
void task_manager_set_event_callback(task_mgr_event_callback_t callback);

#endif