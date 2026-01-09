#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tfnsw_client.h"

// ============================================================================
// Settings Structure
// ============================================================================

typedef struct {
    // Display settings
    uint32_t theme_color;       // RGB888 theme accent color
    uint8_t brightness;         // 0-100 brightness level
    uint8_t default_scene;      // Default scene on boot

    // Departure board data
    char destination[64];
    char calling_stations[128];
    char departure_time[16];
    int departure_mins;
    char next_dest[64];
    char next_time[16];
    char next2_dest[64];
    char next2_time[16];

    // High speed board data
    char hs_destination[64];
    char hs_calling[128];
    char hs_time[16];
    int hs_mins;

    // Internal
    bool loaded;                // Whether settings were loaded from SD
} device_settings_t;

// ============================================================================
// Functions
// ============================================================================

// Initialize settings with defaults
void settings_init(void);

// Load settings from SD card (returns ESP_OK if loaded, ESP_ERR_NOT_FOUND if no file)
esp_err_t settings_load(void);

// Save current settings to SD card
esp_err_t settings_save(void);

// Get current settings (read-only pointer)
const device_settings_t* settings_get(void);

// Update individual settings (auto-saves if SD mounted)
void settings_set_theme_color(uint32_t color);
void settings_set_brightness(uint8_t brightness);
void settings_set_default_scene(uint8_t scene);

// Update departure board settings
void settings_set_departure(const char* dest, const char* calling,
                           const char* time, int mins);
void settings_set_next_departure(const char* dest, const char* time);
void settings_set_next2_departure(const char* dest, const char* time);

// Update high speed board settings
void settings_set_high_speed(const char* dest, const char* calling,
                            const char* time, int mins);

// Check if settings are loaded from SD
bool settings_is_loaded(void);

// Reset settings to defaults
void settings_reset(void);

// ============================================================================
// Departure Cache Functions (SD card based)
// ============================================================================

// Save departures to SD card cache
esp_err_t departures_cache_save(const tfnsw_departures_t* departures);

// Load departures from SD card cache
esp_err_t departures_cache_load(tfnsw_departures_t* out_departures);

// Check if cached departures are still valid (not expired)
bool departures_cache_is_valid(void);

// Clear the departures cache
esp_err_t departures_cache_clear(void);

// ============================================================================
// Logging Functions (SD card based)
// ============================================================================

// Initialize logging to SD card
esp_err_t log_init(void);

// Log a message with timestamp
void log_info(const char* tag, const char* format, ...);
void log_error(const char* tag, const char* format, ...);

// Get current log file size
size_t log_get_size(void);

// Clear log file
esp_err_t log_clear(void);

#endif // SETTINGS_H
