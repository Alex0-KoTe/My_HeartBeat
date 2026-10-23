#ifndef SPIFFS_MANAGER_H
#define SPIFFS_MANAGER_H

#include "esp_err.h"

esp_err_t spiffs_init(void);
esp_err_t spiffs_read_file(const char* path, char* buffer, size_t buffer_size);
esp_err_t spiffs_write_file(const char* path, const char* data, size_t len);
esp_err_t spiffs_list_files(const char* dir);

#endif