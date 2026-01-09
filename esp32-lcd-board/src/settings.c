#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "config.h"
#include "settings.h"
#include "tfnsw_client.h"

static const char *TAG = "settings";

// NVS namespace and keys
#define NVS_SETTINGS_NAMESPACE  "settings"
#define NVS_KEY_THEME_COLOR     "theme_color"
#define NVS_KEY_BRIGHTNESS      "brightness"
#define NVS_KEY_DEFAULT_SCENE   "default_scene"

// Current settings instance
static device_settings_t current_settings;

// ============================================================================
// Default Values
// ============================================================================

static void set_defaults(void)
{
    current_settings.theme_color = 0xFFE000;  // Teal (displays as teal due to BGR swap)
    current_settings.brightness = 20;
    current_settings.default_scene = 1;  // High Speed

    // Metro departure board defaults
    strncpy(current_settings.destination, "Tallawong", sizeof(current_settings.destination));
    strncpy(current_settings.calling_stations,
            "Chatswood, Macquarie Park, Epping, Cherrybrook",
            sizeof(current_settings.calling_stations));
    strncpy(current_settings.departure_time, "07:42", sizeof(current_settings.departure_time));
    current_settings.departure_mins = 2;

    strncpy(current_settings.next_dest, "Sydenham", sizeof(current_settings.next_dest));
    strncpy(current_settings.next_time, "6 min", sizeof(current_settings.next_time));
    strncpy(current_settings.next2_dest, "Tallawong", sizeof(current_settings.next2_dest));
    strncpy(current_settings.next2_time, "10 min", sizeof(current_settings.next2_time));

    // High speed defaults
    strncpy(current_settings.hs_destination, "Newcastle", sizeof(current_settings.hs_destination));
    strncpy(current_settings.hs_calling,
            "Western Sydney Airport, Sydney Central, Gosford, Newcastle",
            sizeof(current_settings.hs_calling));
    strncpy(current_settings.hs_time, "07:42", sizeof(current_settings.hs_time));
    current_settings.hs_mins = 8;

    current_settings.loaded = false;
}

// ============================================================================
// NVS Functions
// ============================================================================

static esp_err_t nvs_save_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u32(handle, NVS_KEY_THEME_COLOR, current_settings.theme_color);
    nvs_set_u8(handle, NVS_KEY_BRIGHTNESS, current_settings.brightness);
    nvs_set_u8(handle, NVS_KEY_DEFAULT_SCENE, current_settings.default_scene);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "Settings saved to NVS");
    }
    return err;
}

static esp_err_t nvs_load_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No NVS settings found, using defaults");
        return err;
    }

    uint32_t theme_color;
    uint8_t brightness, default_scene;

    if (nvs_get_u32(handle, NVS_KEY_THEME_COLOR, &theme_color) == ESP_OK) {
        current_settings.theme_color = theme_color;
    }
    if (nvs_get_u8(handle, NVS_KEY_BRIGHTNESS, &brightness) == ESP_OK) {
        current_settings.brightness = brightness;
    }
    if (nvs_get_u8(handle, NVS_KEY_DEFAULT_SCENE, &default_scene) == ESP_OK) {
        current_settings.default_scene = default_scene;
    }

    nvs_close(handle);
    current_settings.loaded = true;
    ESP_LOGI(TAG, "Settings loaded from NVS");
    return ESP_OK;
}

// ============================================================================
// Settings Management
// ============================================================================

void settings_init(void)
{
    ESP_LOGI(TAG, "Initializing settings");
    set_defaults();
    nvs_load_settings();
}

esp_err_t settings_load(void)
{
    return nvs_load_settings();
}

esp_err_t settings_save(void)
{
    return nvs_save_settings();
}

const device_settings_t* settings_get(void)
{
    return &current_settings;
}

bool settings_is_loaded(void)
{
    return current_settings.loaded;
}

void settings_reset(void)
{
    set_defaults();

    // Clear NVS settings
    nvs_handle_t handle;
    if (nvs_open(NVS_SETTINGS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Settings reset to defaults");
}

// ============================================================================
// Individual Setting Updates
// ============================================================================

void settings_set_theme_color(uint32_t color)
{
    current_settings.theme_color = color;
    nvs_save_settings();
}

void settings_set_brightness(uint8_t brightness)
{
    current_settings.brightness = brightness;
    nvs_save_settings();
}

void settings_set_default_scene(uint8_t scene)
{
    current_settings.default_scene = scene;
    nvs_save_settings();
}

void settings_set_departure(const char* dest, const char* calling,
                           const char* time, int mins)
{
    if (dest) strncpy(current_settings.destination, dest, sizeof(current_settings.destination) - 1);
    if (calling) strncpy(current_settings.calling_stations, calling, sizeof(current_settings.calling_stations) - 1);
    if (time) strncpy(current_settings.departure_time, time, sizeof(current_settings.departure_time) - 1);
    current_settings.departure_mins = mins;
}

void settings_set_next_departure(const char* dest, const char* time)
{
    if (dest) strncpy(current_settings.next_dest, dest, sizeof(current_settings.next_dest) - 1);
    if (time) strncpy(current_settings.next_time, time, sizeof(current_settings.next_time) - 1);
}

void settings_set_next2_departure(const char* dest, const char* time)
{
    if (dest) strncpy(current_settings.next2_dest, dest, sizeof(current_settings.next2_dest) - 1);
    if (time) strncpy(current_settings.next2_time, time, sizeof(current_settings.next2_time) - 1);
}

void settings_set_high_speed(const char* dest, const char* calling,
                            const char* time, int mins)
{
    if (dest) strncpy(current_settings.hs_destination, dest, sizeof(current_settings.hs_destination) - 1);
    if (calling) strncpy(current_settings.hs_calling, calling, sizeof(current_settings.hs_calling) - 1);
    if (time) strncpy(current_settings.hs_time, time, sizeof(current_settings.hs_time) - 1);
    current_settings.hs_mins = mins;
}

// ============================================================================
// Stub functions (previously SD-dependent, now no-ops)
// ============================================================================

esp_err_t log_init(void) { return ESP_OK; }
void log_info(const char* tag, const char* format, ...) { (void)tag; (void)format; }
void log_error(const char* tag, const char* format, ...) { (void)tag; (void)format; }
size_t log_get_size(void) { return 0; }
esp_err_t log_clear(void) { return ESP_OK; }

bool departures_cache_is_valid(void) { return false; }
esp_err_t departures_cache_save(const tfnsw_departures_t* departures) { (void)departures; return ESP_OK; }
esp_err_t departures_cache_load(tfnsw_departures_t* out_departures) { (void)out_departures; return ESP_ERR_NOT_FOUND; }
esp_err_t departures_cache_clear(void) { return ESP_OK; }
