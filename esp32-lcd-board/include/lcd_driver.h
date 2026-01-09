#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tfnsw_client.h"

// ============================================================================
// View Configuration System
// ============================================================================

// Maximum number of views supported
#define MAX_VIEWS 5

// View IDs (matching legacy scene enum for compatibility)
typedef enum {
    VIEW_METRO_NORTH = 0,     // Victoria Cross - Northbound (Tallawong)
    VIEW_METRO_SOUTH,         // Crows Nest - Southbound (Sydenham)
    VIEW_TRAIN_ARTARMON,      // Artarmon - Train services
    VIEW_HIGH_SPEED,          // High speed rail demo
    VIEW_STATUS_INFO,         // System status display
    VIEW_COUNT
} view_id_t;

// Data source types
typedef enum {
    VIEW_DATA_STATIC,         // Demo/hardcoded data
    VIEW_DATA_REALTIME,       // TfNSW API realtime data
} view_data_source_t;

// Display feature flags
typedef struct {
    bool show_train_cars;         // Train visualization (high-speed style)
    bool show_realtime_dot;       // Green/yellow/red status indicator
    bool show_calling_stations;   // Scrolling calling stations text
    bool show_direction_arrow;    // ^ or v direction indicator
    bool show_delay_status;       // "+2m late", "On time", etc.
    bool rotate_header_text;      // Alternating header text
    uint8_t train_car_count;      // Number of train cars (0 = none)
    uint8_t max_following;        // Max following services to show (1-4)
} view_display_opts_t;

// Complete view configuration
typedef struct {
    view_id_t id;                     // Unique view ID
    char name[24];                    // Display name for UI ("Metro North")
    char header_title[24];            // Header bar text ("Victoria Cross")
    char alt_header[24];              // Alternate header for rotation
    char direction_text[16];          // Direction label ("Tallawong")
    uint32_t accent_color;            // Theme color (RGB888)
    uint32_t led_color;               // RGB LED color
    view_data_source_t data_source;   // Static or realtime
    const char* stop_id;              // TfNSW stop ID (NULL for static)
    tfnsw_direction_t direction;      // Direction filter
    view_display_opts_t display;      // Display options
    bool enabled;                     // Include in button cycle
} view_config_t;

// ============================================================================
// View Registry API
// ============================================================================

// Get total number of views
uint8_t lcd_get_view_count(void);

// Get view configuration by ID
const view_config_t* lcd_get_view_config(view_id_t id);

// Get current view ID
view_id_t lcd_get_current_view(void);

// Set current view by ID
void lcd_set_view(view_id_t id);

// Cycle to next enabled view
void lcd_next_view(void);

// Check if view is enabled
bool lcd_is_view_enabled(view_id_t id);

// Update view with departure data
void lcd_update_view_data(view_id_t id, const tfnsw_departures_t* data);

// Clear view data (call when switching views to purge old data)
void lcd_clear_view_data(view_id_t id);

// Clear all view data
void lcd_clear_all_view_data(void);

// Render current view
void lcd_render_current_view(void);

// ============================================================================
// Scene/View Management (Legacy - maps to view system)
// ============================================================================
typedef enum {
    SCENE_METRO_NORTH = VIEW_METRO_NORTH,
    SCENE_METRO_SOUTH = VIEW_METRO_SOUTH,
    SCENE_TRAIN_ARTARMON = VIEW_TRAIN_ARTARMON,
    SCENE_HIGH_SPEED = VIEW_HIGH_SPEED,
    SCENE_STATUS_INFO = VIEW_STATUS_INFO,
    SCENE_COUNT = VIEW_COUNT
} lcd_scene_t;

// Legacy alias
#define SCENE_DEPARTURE_BOARD SCENE_METRO_NORTH

// ============================================================================
// Realtime Status Display
// ============================================================================
typedef enum {
    DISPLAY_STATUS_IDLE,
    DISPLAY_STATUS_CONNECTING,
    DISPLAY_STATUS_FETCHING,
    DISPLAY_STATUS_LIVE,
    DISPLAY_STATUS_ERROR,
    DISPLAY_STATUS_NO_API_KEY,
    DISPLAY_STATUS_NO_SERVICES,
} display_status_t;

// Get current scene
lcd_scene_t lcd_get_current_scene(void);

// Set current scene
void lcd_set_scene(lcd_scene_t scene);

// Cycle to next scene
void lcd_next_scene(void);

// Refresh current scene (re-render with latest data)
void lcd_refresh_scene(void);

// Initialize LCD display with LVGL
esp_err_t lcd_init(void);

// LVGL update - call periodically from main loop
void lcd_update(void);

// Set backlight brightness (0-100)
void lcd_set_backlight(uint8_t brightness);

// Clear screen with color (legacy)
void lcd_clear(uint16_t color);

// Fill rectangle (legacy)
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);

// Draw rectangle outline (legacy)
void lcd_draw_rect(int x, int y, int w, int h, uint16_t color);

// Draw string (legacy)
void lcd_draw_string(int x, int y, const char* str, uint16_t color, uint16_t bg, uint8_t size);

// Draw centered string (legacy)
void lcd_draw_string_centered(int y, const char* str, uint16_t color, uint16_t bg, uint8_t size);

// ============================================================================
// Screen Templates (LVGL)
// ============================================================================

// Show splash screen with progress animation
void lcd_show_splash(void);

// Set IP address to display
void lcd_set_ip(const char* ip);

// Show departure board screen
void lcd_show_departure_board(void);

// Show high speed departure board screen
void lcd_show_high_speed(void);

// Show status/info screen
void lcd_show_status_info(void);

// Set status info data
void lcd_set_wifi_ssid(const char* ssid);
void lcd_set_wifi_rssi(int rssi);
void lcd_set_uptime(uint32_t seconds);

// Set departure board data
void lcd_set_departure_destination(const char* destination);
void lcd_set_departure_calling(const char* calling_stations);
void lcd_set_departure_time(const char* time);
void lcd_set_departure_mins(int mins);
void lcd_set_next_departure(const char* next_time, const char* next_dest);
void lcd_set_next2_departure(const char* next_time, const char* next_dest);

// ============================================================================
// Realtime Data Integration
// ============================================================================

// Update display with realtime departure data
void lcd_update_realtime_departures(const tfnsw_departures_t* departures);

// Update display with dual-direction realtime data (split view)
void lcd_update_dual_departures(const tfnsw_dual_departures_t* departures);

// Simple mode: separate northbound/southbound updates
void lcd_set_simple_mode(bool enabled);
void lcd_update_northbound_departures(const tfnsw_departures_t* departures);
void lcd_update_southbound_departures(const tfnsw_departures_t* departures);

// Set display status (shows indicator in header)
void lcd_set_display_status(display_status_t status);

// Get current display status
display_status_t lcd_get_display_status(void);

// Show API key required screen
void lcd_show_api_key_required(void);

// Show fetching/loading screen
void lcd_show_fetching(void);

// Show sine wave loading screen
void lcd_show_loading(void);

// Show error screen with details
void lcd_show_data_error(const char* title, const char* message, const char* hint);

// Show no services screen
void lcd_show_no_services(const char* message);

// Set realtime indicator visibility
void lcd_set_realtime_indicator(bool is_realtime);

// Set delay indicator for current service
void lcd_set_delay_indicator(int delay_seconds);

// Show WiFi config screen
void lcd_show_wifi_config(const char* ssid, const char* ip);

// Show connecting screen with spinner
void lcd_show_connecting(const char* ssid);

// Show connected screen with large IP
void lcd_show_connected(const char* ssid, const char* ip);

// Show error screen
void lcd_show_error(const char* message);

// ============================================================================
// Theme Management
// ============================================================================

// Set theme accent color (RGB888 hex value 0xRRGGBB)
void lcd_set_theme_accent(uint32_t color);

// Get current theme accent color (returns RGB888)
uint32_t lcd_get_theme_accent(void);

// Preset theme colors (RGB888 values - display has BGR swap)
// Values below show as their labeled color on the display
#define THEME_PRESET_TEAL    0xFFE000  // Displays as teal/cyan
#define THEME_PRESET_BLUE    0xFF8000  // Displays as blue
#define THEME_PRESET_YELLOW  0x00D4FF  // Displays as yellow
#define THEME_PRESET_LIME    0x00FF80  // Displays as lime/yellow-green
#define THEME_PRESET_MAGENTA 0xFF00FF  // Displays as magenta
#define THEME_PRESET_PURPLE  0xFF4444  // Displays as purple/blue
#define THEME_PRESET_WHITE   0xFFFFFF  // Displays as white

#endif // LCD_DRIVER_H
