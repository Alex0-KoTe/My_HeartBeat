#include "web_server.h"
#include "spiffs_manager.h"
#include "lua_engine.h"
#include "task_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

static const char *TAG = "WEB_SERVER";

// ============================================================================
// WebSocket: Управление клиентами
// ============================================================================
#define MAX_WS_CLIENTS 5
static int ws_clients[MAX_WS_CLIENTS] = {0};
static int ws_client_count = 0;
static SemaphoreHandle_t ws_mutex = NULL;
static httpd_handle_t ws_server_handle = NULL;

// Отправка сообщения всем подключённым WebSocket клиентам через POSIX send()
void web_server_ws_broadcast(const char* message) {
    ESP_LOGI(TAG, "BROADCAST CALLED: %s", message);
    
    if (ws_mutex == NULL || ws_client_count == 0) {
        ESP_LOGW(TAG, "Broadcast skipped: mutex=%p, clients=%d", ws_mutex, ws_client_count);
        return;
    }
    
    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    
    // Формируем WebSocket-фрейм вручную
    uint8_t frame[2048];
    int frame_len = 0;
    
    // Byte 0: FIN бит + opcode 0x1 (текстовый фрейм)
    frame[frame_len++] = 0x81;
    
    // Byte 1+: длина payload
    int msg_len = strlen(message);
    if (msg_len < 126) {
        frame[frame_len++] = msg_len;
    } else if (msg_len < 65536) {
        frame[frame_len++] = 0x7E;
        frame[frame_len++] = (msg_len >> 8) & 0xFF;
        frame[frame_len++] = msg_len & 0xFF;
    } else {
        frame[frame_len++] = 0x7F;
        for (int i = 7; i >= 0; i--) {
            frame[frame_len++] = (msg_len >> (i * 8)) & 0xFF;
        }
    }
    
    // Копируем payload
    if (frame_len + msg_len < (int)sizeof(frame)) {
        memcpy(&frame[frame_len], message, msg_len);
        frame_len += msg_len;
    } else {
        ESP_LOGE(TAG, "Message too large for WebSocket frame");
        xSemaphoreGive(ws_mutex);
        return;
    }
    
    // Отправляем всем клиентам
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] != 0) {
            int ret = send(ws_clients[i], frame, frame_len, 0);
            if (ret < 0) {
                ESP_LOGW(TAG, "Failed to send to client %d, closing", ws_clients[i]);
                close(ws_clients[i]);
                ws_clients[i] = 0;
                ws_client_count--;
            } else {
                ESP_LOGI(TAG, "Sent %d bytes to client %d", ret, ws_clients[i]);
            }
        }
    }
    
    xSemaphoreGive(ws_mutex);
}

// ============================================================================
// WebSocket Handler (Надёжное отслеживание клиентов)
// ============================================================================
static esp_err_t ws_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    // 1. Надёжное добавление клиента в список по его fd
    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    
    bool found = false;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] == fd) {
            found = true;
            break;
        }
    }
    
    if (!found) {
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (ws_clients[i] == 0) {
                ws_clients[i] = fd;
                ws_client_count++;
                ESP_LOGI(TAG, "WS client tracked (fd=%d, total=%d)", fd, ws_client_count);
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(ws_mutex);

    // 2. Если это рукопожатие (GET) — просто возвращаем успех
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake completed for fd=%d", fd);
        return ESP_OK;
    }

    // 3. Обработка входящих данных
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Этап А: Получаем размер фрейма
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get frame size");
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    // Этап Б: Выделяем буфер и получаем данные
    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to receive frame data");
        free(buf);
        return ret;
    }

    ESP_LOGI(TAG, "WS received (%d bytes): %.*s", ws_pkt.len, ws_pkt.len, buf);

    // 4. Парсинг команд
    char *cmd = (char *)buf;
    
    if (strncmp(cmd, "run:", 4) == 0) {
        char *filename = cmd + 4;
        task_mgr_config_t config = {
            .task_name = "lua_ws_task",
            .stack_size = 8192,
            .wdt_timeout_ms = 60000,
            .auto_restart = false
        };
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "/scripts/%s", filename);
        
        esp_err_t res = task_manager_run_lua(full_path, &config);
        if (res != ESP_OK) {
            web_server_ws_broadcast("{\"type\":\"error\",\"message\":\"Failed to start\"}");
        }
    } 
    else if (strcmp(cmd, "stop") == 0) {
        task_manager_stop_current();
    } 
    else if (strcmp(cmd, "clear_logs") == 0) {
        task_manager_clear_logs();
        web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"Logs cleared\"}");
    }

    free(buf);
    return ESP_OK;
}

// ============================================================================
// Обработчик статических файлов
// ============================================================================
static esp_err_t file_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "File request: %s", req->uri);
    
    char filepath[1024];
    
    if (strcmp(req->uri, "/") == 0) {
        strcpy(filepath, "/spiffs/index.html");
    } else {
        if (strlen(req->uri) > 1000) {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    const char *content_type = "text/plain";
    if (strstr(filepath, ".html")) content_type = "text/html";
    else if (strstr(filepath, ".css")) content_type = "text/css";
    else if (strstr(filepath, ".js")) content_type = "application/javascript";
    else if (strstr(filepath, ".json")) content_type = "application/json";
    else if (strstr(filepath, ".png")) content_type = "image/png";
    else if (strstr(filepath, ".jpg")) content_type = "image/jpeg";
    
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    
    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunksize);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(fp);
    
    ESP_LOGI(TAG, "Served file: %s", filepath);
    return ESP_OK;
}

// ============================================================================
// API: Список .lua скриптов
// ============================================================================
static esp_err_t api_scripts_handler(httpd_req_t *req) {
    char response[1024];
    int offset = 0;
    
    offset += snprintf(response + offset, sizeof(response) - offset, "[");
    
    DIR *dir = opendir("/spiffs/scripts");
    if (dir != NULL) {
        struct dirent *ent;
        bool first = true;
        
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".lua") == 0) {
                if (!first) {
                    offset += snprintf(response + offset, sizeof(response) - offset, ",");
                }
                offset += snprintf(response + offset, sizeof(response) - offset, "\"%s\"", ent->d_name);
                first = false;
            }
        }
        closedir(dir);
    } else {
        ESP_LOGW(TAG, "Cannot open /spiffs/scripts directory");
    }
    
    offset += snprintf(response + offset, sizeof(response) - offset, "]");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, offset);
    
    return ESP_OK;
}

// ============================================================================
// API: Загрузка Lua-скрипта
// ============================================================================
static esp_err_t api_upload_handler(httpd_req_t *req) {
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing filename\"}");
        return ESP_OK;
    }
    
    char filename[64];
    if (httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing filename parameter\"}");
        return ESP_OK;
    }
    
    size_t len = strlen(filename);
    if (len < 4 || strcmp(filename + len - 4, ".lua") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Only .lua files allowed\"}");
        return ESP_OK;
    }
    
    if (strstr(filename, "..") != NULL || strstr(filename, "/") != NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
        return ESP_OK;
    }
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/spiffs/scripts/%s", filename);
    
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    size_t free_space = total - used;
    
    if (req->content_len > 10240) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"File too large (max 10KB)\"}");
        return ESP_OK;
    }
    
    if (req->content_len > free_space) {
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Not enough space in SPIFFS\"}");
        return ESP_OK;
    }
    
    FILE *fp = fopen(filepath, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to create file\"}");
        return ESP_OK;
    }
    
    char buf[512];
    int remaining = req->content_len;
    int received = 0;
    
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, sizeof(buf));
        if (recv_len <= 0) {
            fclose(fp);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to receive data\"}");
            return ESP_OK;
        }
        fwrite(buf, 1, recv_len, fp);
        remaining -= recv_len;
        received += recv_len;
    }
    
    fclose(fp);
    
    ESP_LOGI(TAG, "Uploaded file: %s (%d bytes)", filename, received);
    
    httpd_resp_set_type(req, "application/json");
    char response[64];
    snprintf(response, sizeof(response), "{\"status\":\"success\",\"size\":%d}", received);
    httpd_resp_sendstr(req, response);
    
    web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"New script uploaded\"}");
    
    return ESP_OK;
}

// ============================================================================
// API: Удаление Lua-скрипта
// ============================================================================
static esp_err_t api_delete_handler(httpd_req_t *req) {
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing filename\"}");
        return ESP_OK;
    }
    
    char filename[64];
    if (httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing filename parameter\"}");
        return ESP_OK;
    }
    
    if (strstr(filename, "..") != NULL || strstr(filename, "/") != NULL || strstr(filename, ".lua") == NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
        return ESP_OK;
    }
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/spiffs/scripts/%s", filename);
    
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "Deleted file: %s", filename);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"File deleted\"}");
        web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"File deleted, refresh list\"}");
    } else {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to delete file\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// API: Запуск скрипта (HTTP fallback)
// ============================================================================
static esp_err_t api_run_handler(httpd_req_t *req) {
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing query\"}");
        return ESP_OK;
    }
    
    char file_val[128];
    if (httpd_query_key_value(query, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing file\"}");
        return ESP_OK;
    }
    
    if (strstr(file_val, "..") != NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid path\"}");
        return ESP_OK;
    }
    
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/scripts/%s", file_val);
    
    task_mgr_config_t config = {
        .task_name = "lua_http_task",
        .stack_size = 8192,
        .wdt_timeout_ms = 60000,
        .auto_restart = false
    };
    
    esp_err_t ret = task_manager_run_lua(full_path, &config);
    
    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"Script started\"}");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Script started\"}");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Task already running\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to start\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// API: Остановка скрипта (HTTP fallback)
// ============================================================================
static esp_err_t api_stop_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    esp_err_t ret = task_manager_stop_current();
    
    if (ret == ESP_OK) {
        web_server_ws_broadcast("{\"type\":\"status\",\"state\":\"idle\",\"message\":\"Script stopped\"}");
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Script stopped\"}");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"No script running\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// API: Получение логов (HTTP fallback для polling)
// ============================================================================
static esp_err_t api_logs_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    
    const char* logs = task_manager_get_logs();
    httpd_resp_sendstr(req, logs ? logs : "");
    
    return ESP_OK;
}

// ============================================================================
// API: Очистка логов
// ============================================================================
static esp_err_t api_logs_clear_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    task_manager_clear_logs();
    httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Logs cleared\"}");
    return ESP_OK;
}

// ============================================================================
// API: Статус текущей задачи
// ============================================================================
static esp_err_t api_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    
    task_mgr_state_t state = task_manager_get_state();
    const char* state_str;
    
    switch (state) {
        case TASK_MGR_STATE_IDLE:              state_str = "idle"; break;
        case TASK_MGR_STATE_RUNNING:           state_str = "running"; break;
        case TASK_MGR_STATE_CRASHED:           state_str = "crashed"; break;
        case TASK_MGR_STATE_MEMORY_ERROR:      state_str = "memory_error"; break;
        case TASK_MGR_STATE_WATCHDOG_TIMEOUT:  state_str = "watchdog_timeout"; break;
        default:                               state_str = "unknown";
    }
    
    char response[128];
    snprintf(response, sizeof(response), "{\"status\":\"%s\"}", state_str);
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

// ============================================================================
// Запуск веб-сервера
// ============================================================================
esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.stack_size = 8192;
    config.max_open_sockets = 7;
    
    if (ws_mutex == NULL) ws_mutex = xSemaphoreCreateMutex();
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return ESP_FAIL;
    
    ws_server_handle = server;
    
    // Статические файлы
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/", .method=HTTP_GET, .handler=file_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/style.css", .method=HTTP_GET, .handler=file_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/script.js", .method=HTTP_GET, .handler=file_handler});
    
    // API
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/scripts", .method=HTTP_GET, .handler=api_scripts_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/upload", .method=HTTP_POST, .handler=api_upload_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/delete", .method=HTTP_POST, .handler=api_delete_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/run", .method=HTTP_GET, .handler=api_run_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/stop", .method=HTTP_GET, .handler=api_stop_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/status", .method=HTTP_GET, .handler=api_status_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/logs", .method=HTTP_GET, .handler=api_logs_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/logs/clear", .method=HTTP_GET, .handler=api_logs_clear_handler});
    
    // WebSocket
    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true
    });
    
    // Связываем Task Manager с WebSocket
    task_manager_set_event_callback(web_server_ws_broadcast);
    
    ESP_LOGI(TAG, "Web server started with WebSocket support");
    return ESP_OK;
}