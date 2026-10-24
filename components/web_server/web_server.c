#include "web_server.h"
#include "spiffs_manager.h"
#include "lua_engine.h"
#include "task_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_SERVER";

// ============================================================================
// Обработчик для раздачи статических файлов из SPIFFS
// ============================================================================
static esp_err_t file_handler(httpd_req_t *req) {
    char filepath[1024];
    
    if (strcmp(req->uri, "/") == 0) {
        strcpy(filepath, "/spiffs/index.html");
    } else {
        if (strlen(req->uri) > 1000) {
            ESP_LOGW(TAG, "URI too long: %s", req->uri);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
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
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    
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
// API: Запуск Lua-скрипта (делегирование Task Manager'у)
// ============================================================================
static esp_err_t api_run_handler(httpd_req_t *req) {
    char query[256];
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing query string\"}");
        return ESP_OK;
    }
    
    char file_val[128];
    if (httpd_query_key_value(query, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing 'file' parameter\"}");
        return ESP_OK;
    }
    
    if (strstr(file_val, "..") != NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
        return ESP_OK;
    }
    
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/scripts/%s", file_val);
    
    ESP_LOGI(TAG, "Request to run script: %s", full_path);
    
    task_mgr_config_t config = {
        .task_name = "lua_web_task",
        .stack_size = 8192,
        .wdt_timeout_ms = 60000,
        .auto_restart = false
    };
    
    esp_err_t ret = task_manager_run_lua(full_path, &config);
    
    httpd_resp_set_type(req, "application/json");
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Script started\"}");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Task already running\"}");
    } else if (ret == ESP_ERR_NO_MEM) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Low memory\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to start\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// API: Получение логов Task Manager (для терминала)
// ============================================================================
static esp_err_t api_logs_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    
    const char* logs = task_manager_get_logs();
    httpd_resp_sendstr(req, logs ? logs : "");
    
    return ESP_OK;
}

// ============================================================================
// Запуск веб-сервера
// ============================================================================
esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size = 8192;
    
    httpd_handle_t server = NULL;
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ESP_FAIL;
    }
    
    // Статические файлы
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &file_uri);
    
    // API эндпоинты
    httpd_uri_t api_scripts_uri = {
        .uri = "/api/scripts",
        .method = HTTP_GET,
        .handler = api_scripts_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scripts_uri);
    
    httpd_uri_t api_run_uri = {
        .uri = "/api/run",
        .method = HTTP_GET,
        .handler = api_run_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_run_uri);
    
    httpd_uri_t api_logs_uri = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_logs_uri);
    
    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}