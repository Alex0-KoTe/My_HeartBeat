#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

// Запуск веб-сервера
esp_err_t web_server_start(void);

// Отправка сообщения всем WebSocket клиентам
void web_server_ws_broadcast(const char* message);

#endif