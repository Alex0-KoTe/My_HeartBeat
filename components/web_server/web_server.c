#include "web_server.h"
#include "spiffs_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "WEB_SERVER";
static httpd_handle_t server = NULL;

// Обработчик главной страницы
static esp_err_t index_handler(httpd_req_t* req) {
    char buffer[4096];
    esp_err_t ret = spiffs_read_file("/index.html", buffer, sizeof(buffer));
    
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read index.html");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

// Обработчик API для получения данных
static esp_err_t api_data_handler(httpd_req_t* req) {
    const char* response = "{\"status\": \"ok\", \"message\": \"Hello from ESP32!\"}";
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Обработчик для загрузки Lua-скриптов
static esp_err_t upload_handler(httpd_req_t* req) {
    char buffer[1024];
    int received = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
        return ESP_FAIL;
    }
    
    buffer[received] = '\0';
    
    // Получаем имя файла из URI
    char filename[256];
    sscanf(req->uri, "/upload/%s", filename);
    
    esp_err_t ret = spiffs_write_file(filename, buffer, received);
    
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save file");
        return ESP_FAIL;
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size = 8192;
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }
    
    // Регистрируем обработчики
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t api_data_uri = {
        .uri = "/api/data",
        .method = HTTP_GET,
        .handler = api_data_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t upload_uri = {
        .uri = "/upload/*",
        .method = HTTP_POST,
        .handler = upload_handler,
        .user_ctx = NULL
    };
    
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &api_data_uri);
    httpd_register_uri_handler(server, &upload_uri);
    
    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
    return ESP_OK;
}