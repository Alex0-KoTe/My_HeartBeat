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

static const char *TAG = "WEB_SERVER";

// ============================================================================
// WebSocket: Управление клиентами (по документации ESP-IDF)
// ============================================================================
#define MAX_WS_CLIENTS 5
static int ws_clients[MAX_WS_CLIENTS] = {0};
static SemaphoreHandle_t ws_mutex = NULL;

// Функция отправки сообщения всем подключенным клиентам
void web_server_ws_broadcast(const char* message) {
    if (ws_mutex == NULL) return;
    
    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)message;
    ws_pkt.len = strlen(message);
    
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i] != 0) {
            // Используем async, чтобы не блокировать сервер
            esp_err_t ret = httpd_ws_send_frame_async(NULL, ws_clients[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to client %d, closing", ws_clients[i]);
                close(ws_clients[i]);
                ws_clients[i] = 0;
            }
        }
    }
    
    xSemaphoreGive(ws_mutex);
}

// ============================================================================
// WebSocket Handler
// ============================================================================
static esp_err_t ws_handler(httpd_req_t *req) {
    // 1. Обработка нового подключения (Handshake)
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        
        xSemaphoreTake(ws_mutex, portMAX_DELAY);
        bool added = false;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (ws_clients[i] == 0) {
                ws_clients[i] = fd;
                added = true;
                ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
                break;
            }
        }
        xSemaphoreGive(ws_mutex);
        
        if (!added) {
            ESP_LOGW(TAG, "Max WS clients reached");
            return ESP_FAIL;
        }
        
        // Возвращаем ESP_OK, чтобы esp_http_server сам выполнил WebSocket Upgrade
        return ESP_OK;
    }

    // 2. Обработка входящих данных от клиента
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
        return ESP_OK; // Пустой фрейм (например, ping)
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

    ESP_LOGI(TAG, "WS received: %.*s", ws_pkt.len, buf);

    // 3. Парсинг команд от браузера
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
// Обработчик статических файлов (без изменений, он работает отлично)
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
    
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    
    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunksize);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(fp);
    
    return ESP_OK;
}

// ============================================================================
// API: Список скриптов
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
                if (!first) offset += snprintf(response + offset, sizeof(response) - offset, ",");
                offset += snprintf(response + offset, sizeof(response) - offset, "\"%s\"", ent->d_name);
                first = false;
            }
        }
        closedir(dir);
    }
    offset += snprintf(response + offset, sizeof(response) - offset, "]");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, offset);
    return ESP_OK;
}

// ============================================================================
// API: Загрузка файла
// ============================================================================
static esp_err_t api_upload_handler(httpd_req_t *req) {
    char query[256], filename[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "filename", filename, sizeof(filename)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing filename\"}");
        return ESP_OK;
    }
    
    if (strstr(filename, "..") != NULL || strstr(filename, ".lua") == NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
        return ESP_OK;
    }
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/spiffs/scripts/%s", filename);
    
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to create file\"}");
        return ESP_OK;
    }
    
    char buf[512];
    int remaining = req->content_len, received = 0;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, sizeof(buf));
        if (recv_len <= 0) break;
        fwrite(buf, 1, recv_len, fp);
        remaining -= recv_len;
        received += recv_len;
    }
    fclose(fp);
    
    httpd_resp_set_type(req, "application/json");
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"success\",\"size\":%d}", received);
    httpd_resp_sendstr(req, resp);
    
    web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"New script uploaded\"}");
    return ESP_OK;
}

// ============================================================================
// API: Запуск и остановка
// ============================================================================
static esp_err_t api_run_handler(httpd_req_t *req) {
    char query[256], file_val[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
        return ESP_OK;
    }
    
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/scripts/%s", file_val);
    
    task_mgr_config_t config = {.task_name = "lua_task", .stack_size = 8192, .wdt_timeout_ms = 60000, .auto_restart = false};
    esp_err_t ret = task_manager_run_lua(full_path, &config);
    
    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        web_server_ws_broadcast("{\"type\":\"status\",\"message\":\"Script started\"}");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_set_status(req, ret == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed\"}");
    }
    return ESP_OK;
}

static esp_err_t api_stop_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (task_manager_stop_current() == ESP_OK) {
        web_server_ws_broadcast("{\"type\":\"status\",\"state\":\"idle\",\"message\":\"Stopped\"}");
        httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    } else {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
    }
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    task_mgr_state_t state = task_manager_get_state();
    const char* str = (state == TASK_MGR_STATE_IDLE) ? "idle" : 
                      (state == TASK_MGR_STATE_RUNNING) ? "running" : "error";
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":\"%s\"}", str);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_logs_clear_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    task_manager_clear_logs();
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");
    return ESP_OK;
}

// ============================================================================
// Запуск сервера
// ============================================================================
esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.stack_size = 8192;
    // Важно для WebSocket: разрешаем больше открытых сокетов
    config.max_open_sockets = 7; 
    
    if (ws_mutex == NULL) ws_mutex = xSemaphoreCreateMutex();
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) return ESP_FAIL;

    // Статика
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/", .method=HTTP_GET, .handler=file_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/style.css", .method=HTTP_GET, .handler=file_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/script.js", .method=HTTP_GET, .handler=file_handler});

    // API
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/scripts", .method=HTTP_GET, .handler=api_scripts_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/upload", .method=HTTP_POST, .handler=api_upload_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/run", .method=HTTP_GET, .handler=api_run_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/stop", .method=HTTP_GET, .handler=api_stop_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/status", .method=HTTP_GET, .handler=api_status_handler});
    httpd_register_uri_handler(server, &(httpd_uri_t){.uri="/api/logs/clear", .method=HTTP_GET, .handler=api_logs_clear_handler});

    // WebSocket
    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true  // <-- Ключевой флаг для ESP-IDF
    });

    // Связываем Task Manager с WebSocket
    task_manager_set_event_callback(web_server_ws_broadcast);

    ESP_LOGI(TAG, "Web server started with WebSocket support");
    return ESP_OK;
}