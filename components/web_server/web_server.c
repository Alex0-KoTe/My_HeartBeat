#include "web_server.h"
#include "spiffs_manager.h"
#include "lua_engine.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "WEB_SERVER";

// ============================================================================
// Обработчик для раздачи статических файлов из SPIFFS
// ============================================================================
static esp_err_t file_handler(httpd_req_t *req) {
    char filepath[1024];  // Увеличенный буфер для безопасности
    
    // Если запрос к корню, отдаем index.html
    if (strcmp(req->uri, "/") == 0) {
        strcpy(filepath, "/spiffs/index.html");
    } else {
        // Строгая проверка длины URI для предотвращения переполнения
        if (strlen(req->uri) > 1000) {
            ESP_LOGW(TAG, "URI too long: %s", req->uri);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }
    
    // Открываем файл
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // Определяем Content-Type по расширению файла
    const char *content_type = "text/plain";
    if (strstr(filepath, ".html")) content_type = "text/html";
    else if (strstr(filepath, ".css")) content_type = "text/css";
    else if (strstr(filepath, ".js")) content_type = "application/javascript";
    else if (strstr(filepath, ".json")) content_type = "application/json";
    else if (strstr(filepath, ".png")) content_type = "image/png";
    else if (strstr(filepath, ".jpg")) content_type = "image/jpeg";
    
    httpd_resp_set_type(req, content_type);
    
    // === ЗАПРЕТ КЭШИРОВАНИЯ (чтобы браузер всегда брал свежий файл) ===
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    // ==================================================================
    
    // Читаем и отправляем файл частями
    char chunk[512];
    size_t chunksize;
    while ((chunksize = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        httpd_resp_send_chunk(req, chunk, chunksize);
    }
    httpd_resp_send_chunk(req, NULL, 0); // Завершаем передачу
    
    fclose(fp);
    ESP_LOGI(TAG, "Served file: %s", filepath);
    return ESP_OK;
}

// ============================================================================
// Обработчик API: получение списка .lua файлов из /scripts
// ============================================================================
static esp_err_t api_scripts_handler(httpd_req_t *req) {
    char response[1024];
    int offset = 0;
    
    // Начинаем JSON-массив
    offset += snprintf(response + offset, sizeof(response) - offset, "[");
    
    DIR *dir = opendir("/spiffs/scripts");
    if (dir != NULL) {
        struct dirent *ent;
        bool first = true;
        
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            // Фильтруем только файлы с расширением .lua
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
    
    // Заканчиваем JSON-массив
    offset += snprintf(response + offset, sizeof(response) - offset, "]");
    
    // ОТПРАВЛЯЕМ ВСЁ ОДНОЙ КОМАНДОЙ (это решает проблему ERR_INVALID_CHUNKED_ENCODING)
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, offset);
    
    return ESP_OK;
}

// ============================================================================
// Обработчик API: запуск Lua-скрипта
// ============================================================================
static esp_err_t api_run_handler(httpd_req_t *req) {
    char query[256];
    
    // Получаем строку запроса
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing query string\"}");
        return ESP_OK;
    }
    
    // Извлекаем параметр file
    char file_val[128];
    if (httpd_query_key_value(query, "file", file_val, sizeof(file_val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing 'file' parameter\"}");
        return ESP_OK;
    }
    
    // Защита от path traversal (запрет на выход из папки)
    if (strstr(file_val, "..") != NULL) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid filename\"}");
        return ESP_OK;
    }
    
    // Формируем полный путь
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/scripts/%s", file_val);
    
    ESP_LOGI(TAG, "Executing script: %s", full_path);
    
    // Вызываем Lua-движок
    esp_err_t ret = lua_engine_execute_file(full_path);
    
    httpd_resp_set_type(req, "application/json");
    
    if (ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Script executed successfully\"}");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to execute script\"}");
    }
    
    return ESP_OK;
}

// ============================================================================
// Функция запуска веб-сервера
// ============================================================================
esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    
    httpd_handle_t server = NULL;
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ESP_FAIL;
    }
    
    // 1. Явный обработчик для корневого пути '/'
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    
    // 2. Обработчик для всех остальных статических файлов
    httpd_uri_t file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &file_uri);
    
    // 3. API: список скриптов
    httpd_uri_t api_scripts_uri = {
        .uri = "/api/scripts",
        .method = HTTP_GET,
        .handler = api_scripts_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scripts_uri);
    
    // 4. API: запуск скрипта
    httpd_uri_t api_run_uri = {
        .uri = "/api/run",
        .method = HTTP_GET,
        .handler = api_run_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_run_uri);
    
    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}