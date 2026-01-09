#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include "esp_err.h"

// Initialize SD card
esp_err_t sd_card_init(void);

// Deinitialize SD card
esp_err_t sd_card_deinit(void);

// Check if SD card is mounted
bool sd_card_is_mounted(void);

// Get card info
uint64_t sd_card_get_total_bytes(void);
uint64_t sd_card_get_free_bytes(void);

// File operations
bool sd_file_exists(const char* path);
esp_err_t sd_read_file(const char* path, char* buffer, size_t buffer_size);
esp_err_t sd_write_file(const char* path, const char* content);
esp_err_t sd_append_file(const char* path, const char* content);
esp_err_t sd_delete_file(const char* path);

#endif // SD_CARD_H
