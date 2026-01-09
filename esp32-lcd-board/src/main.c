#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "cJSON.h"

#include "config.h"
#include "lcd_driver.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "rgb_led.h"
#include "settings.h"
#include "tfnsw_client.h"

static const char *TAG = "main";

// Button handling
static volatile uint32_t last_button_press = 0;
static QueueHandle_t button_event_queue = NULL;

// ============================================================================
// Application State
// ============================================================================

typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_AP,
    APP_STATE_RUNNING,
    APP_STATE_ERROR
} app_state_t;

static app_state_t current_state = APP_STATE_INIT;

// Pending state transitions (set from callbacks, processed in main loop for LVGL safety)
static volatile bool pending_wifi_connected = false;
static volatile bool pending_ap_started = false;
static volatile bool pending_api_key_set = false;

// Forward declarations
static void on_realtime_update(const tfnsw_departures_t* departures);
static void update_brightness_for_time(void);

// Get stop ID for a view
static const char* get_stop_id_for_view(view_id_t view) {
    const view_config_t* config = lcd_get_view_config(view);
    if (config && config->data_source == VIEW_DATA_REALTIME && config->stop_id) {
        return config->stop_id;
    }
    return NULL;
}

// Brightness settings
#define BRIGHTNESS_DAY 80
#define BRIGHTNESS_NIGHT 20
static uint8_t current_brightness = BRIGHTNESS_DAY;
static bool manual_brightness_override = false;  // When true, skip auto-adjustment

// ============================================================================
// Button Handling
// ============================================================================

static void IRAM_ATTR button_isr_handler(void* arg)
{
    uint32_t now = xTaskGetTickCountFromISR();
    if ((now - last_button_press) > pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        last_button_press = now;
        uint32_t gpio_num = (uint32_t)arg;
        xQueueSendFromISR(button_event_queue, &gpio_num, NULL);
    }
}

static void button_task(void* arg)
{
    uint32_t gpio_num;
    while (1) {
        if (xQueueReceive(button_event_queue, &gpio_num, portMAX_DELAY)) {
            (void)gpio_num;  // Unused, but kept for future use

            if (current_state == APP_STATE_RUNNING || current_state == APP_STATE_WIFI_AP) {
                view_id_t old_view = lcd_get_current_view();

                // Switch to next enabled view
                lcd_next_view();

                view_id_t new_view = lcd_get_current_view();
                const view_config_t* new_config = lcd_get_view_config(new_view);
                const view_config_t* old_config = lcd_get_view_config(old_view);

                ESP_LOGI(TAG, "Button pressed - switching from view %d to %d (%s)",
                         old_view, new_view, new_config ? new_config->name : "unknown");

                // Clear old view data when switching
                lcd_clear_view_data(old_view);

                // Check if views need realtime data
                bool new_is_realtime = new_config && new_config->data_source == VIEW_DATA_REALTIME;
                bool old_is_realtime = old_config && old_config->data_source == VIEW_DATA_REALTIME;

                if (new_is_realtime && tfnsw_has_api_key()) {
                    // Switching TO realtime view
                    const char* stop_id = get_stop_id_for_view(new_view);
                    ESP_LOGI(TAG, "Switching to realtime view - stop: %s", stop_id ? stop_id : "(none)");

                    if (!tfnsw_is_background_fetch_running()) {
                        // Start single-view fetch for this stop
                        tfnsw_start_single_view_fetch(stop_id, on_realtime_update);
                    } else {
                        // Just switch the active stop
                        tfnsw_set_active_stop(stop_id);
                    }
                } else if (old_is_realtime && !new_is_realtime) {
                    // Switching AWAY from realtime view - stop fetching and clear data
                    ESP_LOGI(TAG, "Leaving realtime view - stopping fetch");
                    tfnsw_stop_background_fetch();
                    tfnsw_clear_cached_data();
                }

                // LED color is set by lcd_update() when view changes via view config
                // For status view, re-enable status mode
                if (new_view == VIEW_STATUS_INFO) {
                    ESP_LOGI(TAG, "Re-enabling status mode for status view");
                    rgb_led_set_status(rgb_led_get_status());
                }
            }
        }
    }
}

static void init_button(void)
{
    // Create event queue for button presses
    button_event_queue = xQueueCreate(10, sizeof(uint32_t));

    // Configure button GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE  // Trigger on falling edge (button press)
    };
    gpio_config(&io_conf);

    // Install GPIO ISR service and add handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_PIN, button_isr_handler, (void*)BUTTON_PIN);

    // Create button handling task (8192 bytes needed for large tfnsw_dual_departures_t struct)
    xTaskCreate(button_task, "button_task", 8192, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button initialized on GPIO %d", BUTTON_PIN);
}

// ============================================================================
// Time Sync (SNTP) - Non-blocking with background retry
// ============================================================================

static volatile bool sntp_synced = false;
static volatile bool sntp_sync_in_progress = false;

static void sntp_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synchronized successfully");
    sntp_synced = true;
    sntp_sync_in_progress = false;
}

static void init_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP for time sync");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");  // Add backup server
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);
    esp_sntp_init();

    // Set timezone to Sydney/Australia (AEST/AEDT)
    // AEST = UTC+10, AEDT = UTC+11
    // DST starts: First Sunday of October at 2:00am
    // DST ends: First Sunday of April at 3:00am
    setenv("TZ", "AEST-10AEDT,M10.1.0/2,M4.1.0/3", 1);
    tzset();

    sntp_sync_in_progress = true;

    // Brief initial wait (max 3 seconds), then continue anyway
    int retry = 0;
    const int max_initial_wait = 30;  // 3 seconds max initial wait
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < max_initial_wait) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
        sntp_synced = true;
        sntp_sync_in_progress = false;
        ESP_LOGI(TAG, "SNTP time synchronized");
        update_brightness_for_time();
    } else {
        // Continue without blocking - sync will complete in background
        ESP_LOGW(TAG, "SNTP sync pending - continuing with unsynced time");
        ESP_LOGW(TAG, "Time will sync automatically in background");
    }
}

// Check if time is synced (can be called from anywhere)
bool is_time_synced(void)
{
    return sntp_synced || (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET);
}

// ============================================================================
// TfNSW Realtime Update Callback (single-view mode)
// ============================================================================

static void on_realtime_update(const tfnsw_departures_t* departures)
{
    if (!departures) return;

    view_id_t current_view = lcd_get_current_view();
    const view_config_t* config = lcd_get_view_config(current_view);

    // Only update if current view uses realtime data
    if (!config || config->data_source != VIEW_DATA_REALTIME) {
        ESP_LOGW(TAG, "Realtime update received but current view is not realtime");
        return;
    }

    ESP_LOGI(TAG, "Realtime update for view %d - status: %s, count: %d",
             current_view, tfnsw_status_to_string(departures->status), departures->count);

    // Update the current view's data (triggers refresh)
    lcd_update_view_data(current_view, departures);
}

// ============================================================================
// Callbacks
// ============================================================================

static void on_wifi_connected(void)
{
    // Just set flag - actual LVGL operations happen in main loop for thread safety
    ESP_LOGI(TAG, "WiFi connected callback - setting pending flag");
    pending_wifi_connected = true;
}

// Process WiFi connected event - called from main loop (LVGL safe)
static void process_wifi_connected(void)
{
    ESP_LOGI(TAG, "Processing WiFi connected");
    current_state = APP_STATE_RUNNING;

    // Store network info for status display (but don't show it)
    lcd_set_ip(wifi_get_ip());
    lcd_set_wifi_ssid(wifi_get_ssid());
    lcd_set_wifi_rssi(wifi_get_rssi());

    // Initialize time synchronization (required for API timestamps)
    init_sntp();

    // Initialize TfNSW client
    if (tfnsw_init() == ESP_OK) {
        ESP_LOGI(TAG, "TfNSW client initialized");
    } else {
        ESP_LOGE(TAG, "Failed to initialize TfNSW client");
    }

    // Default to high speed view (static/demo data - no fetching needed)
    lcd_set_view(VIEW_HIGH_SPEED);
    ESP_LOGI(TAG, "Starting with High Speed view (static data)");

    // Note: Background fetch will be started when user switches to a realtime view
    // This saves memory and API calls when not needed

    // Start web server
    webserver_start();
}

static void on_ap_started(void)
{
    ESP_LOGI(TAG, "AP mode callback");
    current_state = APP_STATE_WIFI_AP;

    // Solid yellow LED for AP mode
    rgb_led_set_hex(RGB_YELLOW);
}

static void on_api_key_set(void)
{
    // Just set flag - actual LVGL operations happen in main loop for thread safety
    ESP_LOGI(TAG, "API key set callback - setting pending flag");
    pending_api_key_set = true;
}

// Process API key set event - called from main loop (LVGL safe)
static void process_api_key_set(void)
{
    ESP_LOGI(TAG, "Processing API key set");

    view_id_t current_view = lcd_get_current_view();
    const view_config_t* config = lcd_get_view_config(current_view);

    // If currently on a realtime view, start fetching
    if (config && config->data_source == VIEW_DATA_REALTIME) {
        const char* stop_id = get_stop_id_for_view(current_view);
        if (stop_id && !tfnsw_is_background_fetch_running()) {
            ESP_LOGI(TAG, "Starting single-view fetch for current view");
            tfnsw_start_single_view_fetch(stop_id, on_realtime_update);
        } else if (tfnsw_is_background_fetch_running()) {
            ESP_LOGI(TAG, "Background fetch already running - forcing refresh");
            tfnsw_force_refresh();
        }
    } else {
        ESP_LOGI(TAG, "API key set - will fetch when switching to realtime view");
    }
}

static void handle_display_command(const char* command, const char* params)
{
    ESP_LOGI(TAG, "Display command: %s", command);

    if (strcmp(command, "hello_world") == 0) {
        lcd_show_departure_board();
    } else if (strcmp(command, "clear") == 0) {
        lcd_clear(COLOR_BLACK);
    } else if (strcmp(command, "splash") == 0) {
        lcd_show_splash();
    } else if (strcmp(command, "scene") == 0) {
        // Parse scene/view from params JSON
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *scene = cJSON_GetObjectItem(root, "scene");
            if (scene && cJSON_IsNumber(scene)) {
                view_id_t old_view = lcd_get_current_view();
                view_id_t new_view = (view_id_t)scene->valueint;

                if (new_view < VIEW_COUNT) {
                    // Clear old view data when switching
                    lcd_clear_view_data(old_view);

                    lcd_set_view(new_view);
                    settings_set_default_scene((uint8_t)new_view);
                    ESP_LOGI(TAG, "View set to: %d", new_view);

                    // Manage background fetch based on view change
                    const view_config_t* new_config = lcd_get_view_config(new_view);
                    const view_config_t* old_config = lcd_get_view_config(old_view);
                    bool new_is_realtime = new_config && new_config->data_source == VIEW_DATA_REALTIME;
                    bool old_is_realtime = old_config && old_config->data_source == VIEW_DATA_REALTIME;

                    if (new_is_realtime && tfnsw_has_api_key()) {
                        const char* stop_id = get_stop_id_for_view(new_view);
                        if (!tfnsw_is_background_fetch_running()) {
                            tfnsw_start_single_view_fetch(stop_id, on_realtime_update);
                        } else {
                            tfnsw_set_active_stop(stop_id);
                        }
                    } else if (old_is_realtime && !new_is_realtime) {
                        tfnsw_stop_background_fetch();
                        tfnsw_clear_cached_data();
                    }

                    // LED color and theme are set by lcd_update() via view config
                    // For status view, re-enable status mode
                    if (new_view == VIEW_STATUS_INFO) {
                        rgb_led_set_status(rgb_led_get_status());
                    }
                }
            }
            cJSON_Delete(root);
        }
    } else if (strcmp(command, "theme") == 0) {
        // Parse theme color from params JSON
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *color = cJSON_GetObjectItem(root, "color");
            if (color && cJSON_IsNumber(color)) {
                uint32_t color_val = (uint32_t)color->valueint;
                lcd_set_theme_accent(color_val);
                settings_set_theme_color(color_val);  // Save to SD
                ESP_LOGI(TAG, "Theme color set to: 0x%06X", (unsigned int)color_val);
                log_info(TAG, "Theme changed to 0x%06X", (unsigned int)color_val);
            }
            cJSON_Delete(root);
        }
    } else if (strcmp(command, "brightness") == 0) {
        // Parse brightness from params JSON
        ESP_LOGI(TAG, "Brightness command received, params: %s", params ? params : "(null)");
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *level = cJSON_GetObjectItem(root, "level");
            if (level && cJSON_IsNumber(level)) {
                uint8_t brightness = (uint8_t)level->valueint;
                ESP_LOGI(TAG, "Setting brightness to %d%%", brightness);
                current_brightness = brightness;
                manual_brightness_override = true;  // Disable auto-adjustment
                lcd_set_backlight(brightness);
                settings_set_brightness(brightness);
            } else {
                ESP_LOGW(TAG, "Missing or invalid 'level' in brightness command");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "Failed to parse brightness params JSON");
        }
    } else if (strcmp(command, "set_departure") == 0) {
        // Parse departure data from params JSON
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *dest = cJSON_GetObjectItem(root, "destination");
            cJSON *calling = cJSON_GetObjectItem(root, "calling");
            cJSON *time = cJSON_GetObjectItem(root, "time");
            cJSON *mins = cJSON_GetObjectItem(root, "mins");

            const char* dest_str = (dest && cJSON_IsString(dest)) ? dest->valuestring : NULL;
            const char* calling_str = (calling && cJSON_IsString(calling)) ? calling->valuestring : NULL;
            const char* time_str = (time && cJSON_IsString(time)) ? time->valuestring : NULL;
            int mins_val = (mins && cJSON_IsNumber(mins)) ? mins->valueint : 0;

            if (dest_str) lcd_set_departure_destination(dest_str);
            if (calling_str) lcd_set_departure_calling(calling_str);
            if (time_str) lcd_set_departure_time(time_str);
            lcd_set_departure_mins(mins_val);

            settings_set_departure(dest_str, calling_str, time_str, mins_val);
            lcd_refresh_scene();

            cJSON_Delete(root);
        }
    } else if (strcmp(command, "text") == 0) {
        // Parse text parameters
        cJSON *root = cJSON_Parse(params);
        if (root) {
            cJSON *text = cJSON_GetObjectItem(root, "text");
            cJSON *x = cJSON_GetObjectItem(root, "x");
            cJSON *y = cJSON_GetObjectItem(root, "y");
            cJSON *size = cJSON_GetObjectItem(root, "size");

            if (text && cJSON_IsString(text)) {
                int px = (x && cJSON_IsNumber(x)) ? x->valueint : 0;
                int py = (y && cJSON_IsNumber(y)) ? y->valueint : 0;
                int ps = (size && cJSON_IsNumber(size)) ? size->valueint : 2;

                lcd_clear(COLOR_BLACK);
                lcd_draw_string(px, py, text->valuestring, COLOR_WHITE, COLOR_BLACK, ps);
            }
            cJSON_Delete(root);
        }
    }
}

static void handle_system_command(const char* command)
{
    ESP_LOGI(TAG, "System command: %s", command);

    if (strcmp(command, "sleep") == 0) {
        lcd_set_backlight(0);
    } else if (strcmp(command, "wake") == 0) {
        lcd_set_backlight(current_brightness);
    }
}

// Update brightness based on time of day (dim between 19:00 and 08:00)
static void update_brightness_for_time(void)
{
    // Skip if user has manually set brightness
    if (manual_brightness_override) {
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Check if time is synced (year > 2020)
    if (timeinfo.tm_year < 120) {
        return;  // Time not synced yet
    }

    int hour = timeinfo.tm_hour;
    uint8_t target_brightness;

    // Dim between 19:00 (7 PM) and 08:00 (8 AM)
    if (hour >= 19 || hour < 8) {
        target_brightness = BRIGHTNESS_NIGHT;
    } else {
        target_brightness = BRIGHTNESS_DAY;
    }

    // Only update if brightness changed
    if (target_brightness != current_brightness) {
        current_brightness = target_brightness;
        lcd_set_backlight(current_brightness);
        ESP_LOGI(TAG, "Brightness adjusted to %d%% (hour: %d)", current_brightness, hour);
    }
}

// ============================================================================
// Initialization
// ============================================================================

static void init_hardware(void)
{
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "%s", BOARD_NAME);
    ESP_LOGI(TAG, "Firmware v%s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "================================");

    // Initialize RGB LED first (for status indication)
    rgb_led_init();

    // Initialize button
    init_button();

    // Initialize LCD
    ESP_ERROR_CHECK(lcd_init());
    lcd_show_splash();

    // Show splash for 2.5 seconds with pulsing white LED
    for (int i = 0; i < 250; i++) {
        lcd_update();

        // Pulse white LED using sine wave (0-25 brightness range)
        // One full pulse cycle over ~1.25 seconds (125 iterations)
        float angle = (float)i * 3.14159f * 2.0f / 125.0f;
        uint8_t brightness = (uint8_t)(12 + 12 * sinf(angle));  // Range 0-24
        rgb_led_set_color(brightness, brightness, brightness);

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Initialize settings with defaults (NVS-based, no SD card)
    settings_init();

    // Apply saved brightness if available
    const device_settings_t* cfg = settings_get();
    if (cfg && cfg->brightness > 0) {
        current_brightness = cfg->brightness;
        manual_brightness_override = true;  // User had set a custom brightness
        lcd_set_backlight(current_brightness);
        ESP_LOGI(TAG, "Restored saved brightness: %d%%", current_brightness);
    }

    // SD card disabled to avoid SPI conflicts with display
    // Settings are loaded from NVS instead
    ESP_LOGI(TAG, "Using NVS-based settings (SD card disabled)");

    // Initialize WiFi
    ESP_ERROR_CHECK(wifi_init());
    wifi_set_connected_callback(on_wifi_connected);
    wifi_set_ap_callback(on_ap_started);

    // Set up web server callbacks
    webserver_set_display_callback(handle_display_command);
    webserver_set_system_callback(handle_system_command);
    webserver_set_api_key_callback(on_api_key_set);
}

// WiFi connection timeout in milliseconds
#define WIFI_CONNECT_TIMEOUT_MS 30000

static void connect_network(void)
{
    current_state = APP_STATE_WIFI_CONNECTING;
    lcd_show_loading();  // Show sine wave loading instead of WiFi info
    rgb_led_set_hex(RGB_YELLOW);  // Solid yellow during startup

    ESP_LOGI(TAG, "Attempting WiFi connection (timeout: %d seconds)", WIFI_CONNECT_TIMEOUT_MS / 1000);

    esp_err_t ret = wifi_connect();

    if (ret == ESP_OK && wifi_is_connected()) {
        // Connected to saved network - callback will handle the rest
        ESP_LOGI(TAG, "WiFi connected successfully");
        return;
    }

    // Connection failed or timed out - fall back to AP mode
    ESP_LOGW(TAG, "WiFi connection failed, starting AP mode");
    current_state = APP_STATE_WIFI_AP;

    // Show AP mode info briefly on display
    lcd_show_wifi_config(WIFI_AP_SSID, "192.168.4.1");
    rgb_led_set_hex(RGB_YELLOW);  // Yellow for AP mode

    // Start web server for configuration
    webserver_start();

    // After 5 seconds, go back to loading screen (cleaner look)
    vTaskDelay(pdMS_TO_TICKS(5000));
    lcd_show_loading();
}

// ============================================================================
// Main Application
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "Starting application...");

    init_hardware();
    connect_network();

    // Main loop - 10ms tick rate for smooth animations
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t loop_count = 0;

    while (1) {
        // Process pending WiFi state changes (LVGL-safe: runs in main loop)
        if (pending_wifi_connected) {
            pending_wifi_connected = false;
            process_wifi_connected();
        }

        // Process pending API key set (LVGL-safe: runs in main loop)
        if (pending_api_key_set) {
            pending_api_key_set = false;
            process_api_key_set();
        }

        // Update LVGL (handles animations, rendering, pending scene/realtime updates)
        lcd_update();

        // Update LED status animation
        rgb_led_update();

        // Periodic tasks (every ~1 second)
        if (loop_count % 100 == 0) {
            switch (current_state) {
                case APP_STATE_RUNNING:
                    // Update WiFi RSSI for status display
                    lcd_set_wifi_rssi(wifi_get_rssi());
                    lcd_set_uptime(esp_timer_get_time() / 1000000);
                    break;

                case APP_STATE_WIFI_AP:
                    // In AP mode waiting for configuration
                    break;

                case APP_STATE_ERROR:
                    // Error state - wait for reset
                    break;

                default:
                    break;
            }
        }

        // Check brightness every ~10 seconds
        if (loop_count % 1000 == 0) {
            update_brightness_for_time();
        }

        loop_count++;

        // Fixed 10ms tick for consistent animation speed
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
    }
}
