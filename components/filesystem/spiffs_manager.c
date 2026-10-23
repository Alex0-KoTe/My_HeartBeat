#include "spiffs_manager.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char* TAG = "SPIFFS";

esp_err_t spiffs_init(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS info");
    } else {
        ESP_LOGI(TAG, "SPIFFS size: %d bytes, used: %d bytes", total, used);
    }
    
    return ESP_OK;
}

esp_err_t spiffs_read_file(const char* path, char* buffer, size_t buffer_size) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/spiffs%s", path);
    
    FILE* f = fopen(full_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return ESP_FAIL;
    }
    
    size_t read = fread(buffer, 1, buffer_size - 1, f);
    buffer[read] = '\0';
    fclose(f);
    
    ESP_LOGI(TAG, "Read %d bytes from %s", read, full_path);
    return ESP_OK;
}

esp_err_t spiffs_write_file(const char* path, const char* data, size_t len) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/spiffs%s", path);
    
    FILE* f = fopen(full_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return ESP_FAIL;
    }
    
    fwrite(data, 1, len, f);
    fclose(f);
    
    ESP_LOGI(TAG, "Wrote %d bytes to %s", len, full_path);
    return ESP_OK;
}

esp_err_t spiffs_list_files(const char* dir) {
    char full_path[256];
    snprintf(full_path, sizeof(full_path), "/spiffs%s", dir);
    
    DIR* d = opendir(full_path);
    if (!d) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return ESP_FAIL;
    }
    
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        ESP_LOGI(TAG, "File: %s", entry->d_name);
    }
    
    closedir(d);
    return ESP_OK;
}