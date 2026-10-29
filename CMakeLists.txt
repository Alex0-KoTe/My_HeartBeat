cmake_minimum_required(VERSION 3.16)

# Создаём образ SPIFFS из папки spiffs_image
# Это нужно ДО объявления project()
set(SPIFFS_IMAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_image)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_heartbeat)
message(STATUS "SPIFFS_IMAGE_DIR = ${SPIFFS_IMAGE_DIR}")
file(GLOB_RECURSE SPIFFS_FILES "${SPIFFS_IMAGE_DIR}/*")
message(STATUS "SPIFFS files found: ${SPIFFS_FILES}")
# Создаём партицию SPIFFS после объявления проекта
spiffs_create_partition_image(spiffs ${SPIFFS_IMAGE_DIR} FLASH_IN_PROJECT)