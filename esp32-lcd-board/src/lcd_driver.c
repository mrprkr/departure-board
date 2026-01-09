#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "lvgl.h"
#include "config.h"
#include "lcd_driver.h"
#include "tfnsw_client.h"
#include "rgb_led.h"

static const char *TAG = "lcd_driver";

// LVGL display and driver
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_disp_t *disp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;

// LVGL buffer
#define LVGL_BUF_SIZE (LCD_WIDTH * 40)
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

// Scene management
static lcd_scene_t current_scene = SCENE_HIGH_SPEED;
static volatile int pending_scene = -1;  // -1 = no pending change
static volatile uint32_t pending_theme = 0;
static volatile bool theme_change_pending = false;

// Dynamic theme accent color (RGB888 format for lv_color_hex)
static uint32_t theme_accent_color = 0xFFE000;  // Default teal (displays as teal due to BGR swap)

// Per-scene default colors (RGB888 values - these are swapped by display hardware)
// 0xFFE000 displays as teal/cyan, 0x00D4FF displays as yellow
#define SCENE_COLOR_METRO_NORTH 0xFFE000  // Teal on display
#define SCENE_COLOR_METRO_SOUTH 0xFF8000  // Blue on display
#define SCENE_COLOR_HIGH_SPEED  0x00D4FF  // Yellow on display
#define SCENE_COLOR_STATUS      0xFFE000  // Teal on display

// Legacy alias
#define SCENE_COLOR_METRO SCENE_COLOR_METRO_NORTH

// Status data storage
static char current_ip[32] = "0.0.0.0";
static char current_ssid[33] = "";
static int current_rssi = 0;
static uint32_t current_uptime = 0;

// Departure board data storage
static char departure_destination[64] = "Tallawong";
static char departure_calling[128] = "Chatswood, Macquarie Park, Epping, Cherrybrook";
static char departure_time[16] = "07:42";
static int departure_mins = 2;
static char next_departure_time[16] = "6 min";
static char next_departure_dest[64] = "Sydenham";
static char next2_departure_time[16] = "10 min";
static char next2_departure_dest[64] = "Tallawong";

// Realtime data state
static display_status_t current_display_status = DISPLAY_STATUS_IDLE;
static bool is_realtime_data = false;
static int current_delay_seconds = 0;
static tfnsw_departures_t realtime_departures = {0};
static tfnsw_dual_departures_t dual_departures = {0};

// Simple mode data
static tfnsw_departures_t northbound_data = {0};
static tfnsw_departures_t southbound_data = {0};
static bool simple_mode_enabled = false;
static volatile bool pending_north_update = false;
static volatile bool pending_south_update = false;

// High speed view rotation state
static lv_timer_t *hs_rotation_timer = NULL;
static lv_obj_t *hs_header_label = NULL;
static lv_obj_t *hs_time_label = NULL;
static lv_obj_t *hs_dest_label = NULL;       // Main destination label
static lv_obj_t *hs_mins_label = NULL;       // Minutes to departure label
static lv_obj_t *hs_calling_label = NULL;    // Scrolling calling stations label
static bool hs_show_alt_text = false;

// High-speed service data structure
typedef struct {
    const char* destination;
    const char* calling_stations;
    int mins_to_departure;
    int train_cars;
    const char* info_text;  // Train length and other info
} hs_service_t;

// High-speed services data (sorted by departure time - soonest first)
static const hs_service_t hs_services[] = {
    {
        .destination = "Newcastle HSR",
        .calling_stations = "Central Coast HSR, Newcastle HSR",
        .mins_to_departure = 8,
        .train_cars = 9,
        .info_text = "A high-speed service formed of 9 cars. Unreserved seating in cars 6-9.",
    },
    {
        .destination = "West. Syd Intl.",
        .calling_stations = "Parramatta HSR, Western Sydney Intl",
        .mins_to_departure = 22,
        .train_cars = 6,
        .info_text = "A high-speed service formed of 6 cars. Airport express service.",
    },
    {
        .destination = "Newcastle HSR",
        .calling_stations = "Gosford HSR, Central Coast HSR, Newcastle HSR",
        .mins_to_departure = 38,
        .train_cars = 9,
        .info_text = "A high-speed service formed of 9 cars. Quiet car available in car 1.",
    },
    {
        .destination = "Newcastle HSR",
        .calling_stations = "Central Coast HSR, Newcastle HSR",
        .mins_to_departure = 52,
        .train_cars = 12,
        .info_text = "A high-speed service formed of 12 cars. Dining car available.",
    },
};
#define HS_SERVICE_COUNT (sizeof(hs_services) / sizeof(hs_services[0]))
static bool realtime_mode_enabled = false;
static bool dual_mode_enabled = false;
static volatile bool pending_realtime_update = false;  // Thread-safe update flag
static volatile bool pending_dual_update = false;      // Thread-safe update flag for dual mode

// Forward declarations
static void lcd_show_realtime_metro_board(void);
static void lcd_show_dual_metro_board(void);
static void lcd_show_simple_metro_board(bool northbound);
static void lcd_apply_realtime_update(void);
static void lcd_apply_dual_update(void);
static void lcd_apply_simple_update(bool northbound);
static const char* get_current_time_str(void);
static void lcd_render_departure_view(const view_config_t* config, const tfnsw_departures_t* data);

// ============================================================================
// View Registry - Predefined Views
// ============================================================================

static const view_config_t view_registry[VIEW_COUNT] = {
    // VIEW_METRO_NORTH - Victoria Cross Northbound
    {
        .id = VIEW_METRO_NORTH,
        .name = "Metro North",
        .header_title = "Victoria Cross",
        .alt_header = "",
        .direction_text = "Tallawong",
        .accent_color = 0xFFE000,   // Teal
        .led_color = 0x00FFFF,      // RGB_CYAN
        .data_source = VIEW_DATA_REALTIME,
        .stop_id = "206046",
        .direction = TFNSW_DIRECTION_NORTHBOUND,
        .display = {
            .show_train_cars = false,
            .show_realtime_dot = true,
            .show_calling_stations = false,
            .show_direction_arrow = true,
            .show_delay_status = true,
            .rotate_header_text = false,
            .train_car_count = 0,
            .max_following = 3,
        },
        .enabled = true,
    },
    // VIEW_METRO_SOUTH - Crows Nest Southbound
    {
        .id = VIEW_METRO_SOUTH,
        .name = "Metro South",
        .header_title = "Crows Nest",
        .alt_header = "",
        .direction_text = "Sydenham",
        .accent_color = 0xFF8000,   // Blue
        .led_color = 0x0080FF,
        .data_source = VIEW_DATA_REALTIME,
        .stop_id = "206037",
        .direction = TFNSW_DIRECTION_SOUTHBOUND,
        .display = {
            .show_train_cars = false,
            .show_realtime_dot = true,
            .show_calling_stations = false,
            .show_direction_arrow = true,
            .show_delay_status = true,
            .rotate_header_text = false,
            .train_car_count = 0,
            .max_following = 3,
        },
        .enabled = true,
    },
    // VIEW_TRAIN_ARTARMON - Sydney Trains from Artarmon
    {
        .id = VIEW_TRAIN_ARTARMON,
        .name = "Artarmon",
        .header_title = "Artarmon",
        .alt_header = "",
        .direction_text = "",
        .accent_color = 0x00FF80,   // Orange/lime (train color)
        .led_color = 0xFF8000,      // Orange LED
        .data_source = VIEW_DATA_REALTIME,
        .stop_id = "10101116",
        .direction = TFNSW_DIRECTION_UNKNOWN,  // Show all directions
        .display = {
            .show_train_cars = false,
            .show_realtime_dot = true,
            .show_calling_stations = false,
            .show_direction_arrow = false,
            .show_delay_status = true,
            .rotate_header_text = false,
            .train_car_count = 0,
            .max_following = 3,
        },
        .enabled = false,  // Disabled: Sydney Trains API responses too large (32KB+) for ESP32 memory
    },
    // VIEW_HIGH_SPEED - Demo high-speed rail
    {
        .id = VIEW_HIGH_SPEED,
        .name = "High Speed",
        .header_title = "High Speed",
        .alt_header = "Go to platform",
        .direction_text = "",
        .accent_color = 0x00D4FF,   // Yellow
        .led_color = 0xFFFF00,      // RGB_YELLOW
        .data_source = VIEW_DATA_STATIC,
        .stop_id = NULL,
        .direction = TFNSW_DIRECTION_UNKNOWN,
        .display = {
            .show_train_cars = true,
            .show_realtime_dot = false,
            .show_calling_stations = true,
            .show_direction_arrow = false,
            .show_delay_status = false,
            .rotate_header_text = true,
            .train_car_count = 9,
            .max_following = 3,
        },
        .enabled = true,
    },
    // VIEW_STATUS_INFO - System status
    {
        .id = VIEW_STATUS_INFO,
        .name = "Status",
        .header_title = "Status",
        .alt_header = "",
        .direction_text = "",
        .accent_color = 0xFFE000,   // Teal
        .led_color = 0x00FFFF,
        .data_source = VIEW_DATA_STATIC,
        .stop_id = NULL,
        .direction = TFNSW_DIRECTION_UNKNOWN,
        .display = {
            .show_train_cars = false,
            .show_realtime_dot = false,
            .show_calling_stations = false,
            .show_direction_arrow = false,
            .show_delay_status = false,
            .rotate_header_text = false,
            .train_car_count = 0,
            .max_following = 0,
        },
        .enabled = true,
    },
};

// View data storage (one per view) - volatile for thread safety
static volatile tfnsw_departures_t view_data[VIEW_COUNT] = {0};
static volatile bool view_data_pending[VIEW_COUNT] = {false};

// Current view tracking
static view_id_t current_view = VIEW_HIGH_SPEED;

// Static demo data for high-speed view
static const tfnsw_departures_t highspeed_demo_data = {
    .count = 4,
    .station_name = "Sydney HSR",
    .status = TFNSW_STATUS_SUCCESS,
    .departures = {
        {
            .destination = "West. Syd Intl.",
            .mins_to_departure = 2,
            .is_realtime = false,
            .occupancy_percent = 65,
            .calling_stations = "Parramatta",
        },
        {
            .destination = "Sydney HSR",
            .mins_to_departure = 8,
            .is_realtime = false,
            .occupancy_percent = 45,
        },
        {
            .destination = "Central Coast",
            .mins_to_departure = 15,
            .is_realtime = false,
            .occupancy_percent = 80,
        },
        {
            .destination = "Newcastle HSR",
            .mins_to_departure = 22,
            .is_realtime = false,
            .occupancy_percent = 30,
        },
    },
};

// Demo data for Metro North (Victoria Cross -> Tallawong)
static const tfnsw_departures_t metro_north_demo_data = {
    .count = 4,
    .station_name = "Victoria Cross",
    .status = TFNSW_STATUS_SUCCESS,
    .departures = {
        {
            .destination = "Tallawong",
            .mins_to_departure = 3,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_NORTHBOUND,
        },
        {
            .destination = "Tallawong",
            .mins_to_departure = 7,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_NORTHBOUND,
        },
        {
            .destination = "Tallawong",
            .mins_to_departure = 11,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_NORTHBOUND,
        },
        {
            .destination = "Tallawong",
            .mins_to_departure = 15,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_NORTHBOUND,
        },
    },
};

// Demo data for Metro South (Crows Nest -> Sydenham)
static const tfnsw_departures_t metro_south_demo_data = {
    .count = 4,
    .station_name = "Crows Nest",
    .status = TFNSW_STATUS_SUCCESS,
    .departures = {
        {
            .destination = "Sydenham",
            .mins_to_departure = 2,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_SOUTHBOUND,
        },
        {
            .destination = "Sydenham",
            .mins_to_departure = 6,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_SOUTHBOUND,
        },
        {
            .destination = "Sydenham",
            .mins_to_departure = 10,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_SOUTHBOUND,
        },
        {
            .destination = "Sydenham",
            .mins_to_departure = 14,
            .is_realtime = false,
            .direction = TFNSW_DIRECTION_SOUTHBOUND,
        },
    },
};

// ============================================================================
// View Registry API Implementation
// ============================================================================

uint8_t lcd_get_view_count(void)
{
    return VIEW_COUNT;
}

const view_config_t* lcd_get_view_config(view_id_t id)
{
    if (id >= VIEW_COUNT) return NULL;
    return &view_registry[id];
}

view_id_t lcd_get_current_view(void)
{
    return current_view;
}

void lcd_set_view(view_id_t id)
{
    if (id >= VIEW_COUNT) return;
    pending_scene = (int)id;  // Use existing pending mechanism
}

void lcd_next_view(void)
{
    // Find next enabled view
    view_id_t next = (current_view + 1) % VIEW_COUNT;
    int attempts = 0;
    while (!view_registry[next].enabled && attempts < VIEW_COUNT) {
        next = (next + 1) % VIEW_COUNT;
        attempts++;
    }
    lcd_set_view(next);
}

bool lcd_is_view_enabled(view_id_t id)
{
    if (id >= VIEW_COUNT) return false;
    return view_registry[id].enabled;
}

void lcd_update_view_data(view_id_t id, const tfnsw_departures_t* data)
{
    if (id >= VIEW_COUNT || !data) return;
    memcpy((void*)&view_data[id], data, sizeof(tfnsw_departures_t));
    view_data_pending[id] = true;
    ESP_LOGI("LCD", "View %d data updated: count=%d, status=%d", id, data->count, data->status);
}

void lcd_clear_view_data(view_id_t id)
{
    if (id >= VIEW_COUNT) return;
    memset((void*)&view_data[id], 0, sizeof(tfnsw_departures_t));
    view_data_pending[id] = false;
    ESP_LOGI("LCD", "View %d data cleared", id);
}

void lcd_clear_all_view_data(void)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        memset((void*)&view_data[i], 0, sizeof(tfnsw_departures_t));
        view_data_pending[i] = false;
    }
    ESP_LOGI("LCD", "All view data cleared");
}

// Get demo data for a specific view (used as fallback when realtime unavailable)
static const tfnsw_departures_t* get_demo_data_for_view(view_id_t view)
{
    switch (view) {
        case VIEW_METRO_NORTH:
            return &metro_north_demo_data;
        case VIEW_METRO_SOUTH:
            return &metro_south_demo_data;
        case VIEW_HIGH_SPEED:
        default:
            return &highspeed_demo_data;
    }
}

// Check if realtime data is valid and usable
static bool is_realtime_data_valid(const tfnsw_departures_t* data)
{
    if (!data) return false;
    if (data->count == 0) return false;
    if (data->status != TFNSW_STATUS_SUCCESS &&
        data->status != TFNSW_STATUS_SUCCESS_CACHED) return false;
    return true;
}

void lcd_render_current_view(void)
{
    const view_config_t* config = lcd_get_view_config(current_view);
    if (!config) return;

    ESP_LOGI("LCD", "Rendering view %d (%s)", current_view, config->name);

    // Set theme color for this view
    theme_accent_color = config->accent_color;

    // Special case for status view
    if (current_view == VIEW_STATUS_INFO) {
        lcd_show_status_info();
        return;
    }

    // Special case for high-speed view (has its own service cycling and animations)
    if (current_view == VIEW_HIGH_SPEED) {
        lcd_show_high_speed();
        return;
    }

    // Get appropriate data
    const tfnsw_departures_t* data;
    bool using_demo = false;

    if (config->data_source == VIEW_DATA_STATIC) {
        data = get_demo_data_for_view(current_view);
        using_demo = true;
    } else {
        // Try realtime data first
        const tfnsw_departures_t* realtime_data = (const tfnsw_departures_t*)&view_data[current_view];

        if (is_realtime_data_valid(realtime_data)) {
            data = realtime_data;
            ESP_LOGI("LCD", "Using realtime data: count=%d, status=%d", data->count, data->status);
        } else {
            // Fallback to demo data
            data = get_demo_data_for_view(current_view);
            using_demo = true;
            ESP_LOGI("LCD", "Realtime unavailable (count=%d, status=%d), using demo data",
                     realtime_data->count, realtime_data->status);
        }
    }

    lcd_render_departure_view(config, data);

    // If using demo data, the status dot will show yellow (scheduled only)
    if (using_demo) {
        ESP_LOGI("LCD", "View rendered with demo/fallback data");
    }
}

// ============================================================================
// UI Component Functions (DRY)
// ============================================================================

// Header bar timer state (for rotation)
static lv_timer_t *view_rotation_timer = NULL;
static lv_obj_t *view_header_label = NULL;
static lv_obj_t *view_time_label = NULL;
static bool view_show_alt_text = false;
static const view_config_t* current_render_config = NULL;

// Header rotation timer callback
static void view_rotation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    view_show_alt_text = !view_show_alt_text;

    if (current_render_config && view_header_label && lv_obj_is_valid(view_header_label)) {
        if (view_show_alt_text && current_render_config->alt_header[0]) {
            lv_label_set_text(view_header_label, current_render_config->alt_header);
        } else {
            lv_label_set_text(view_header_label, current_render_config->header_title);
        }
    }
}

// Render header bar with title and time
static void render_header(lv_obj_t* scr, const view_config_t* config, bool show_fetching)
{
    // Ensure screen has no scrollbars
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header background
    lv_obj_t *header_bg = lv_obj_create(scr);
    lv_obj_remove_style_all(header_bg);
    lv_obj_set_scrollbar_mode(header_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(header_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header_bg, LCD_WIDTH, 24);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(config->accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    // Header title
    view_header_label = lv_label_create(scr);
    lv_label_set_text(view_header_label, config->header_title);
    lv_obj_set_style_text_font(view_header_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(view_header_label, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(view_header_label, 8, 4);

    // Status indicators (right side)
    int indicator_x = LCD_WIDTH - 58;

    // Refresh icon when fetching
    if (show_fetching && tfnsw_is_fetching()) {
        lv_obj_t *refresh_icon = lv_label_create(scr);
        lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(refresh_icon, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(refresh_icon, lv_color_hex(THEME_BG), 0);
        lv_obj_set_pos(refresh_icon, indicator_x, 5);
        indicator_x += 14;
    }

    // Current time
    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 4);
}

// Render status indicator dot
// Only shows green when:
// 1. Status is SUCCESS (not cached, not error)
// 2. Has realtime data (estimated_time available)
// 3. Data count > 0 (actual departures parsed)
static void render_status_dot(lv_obj_t* scr, int x, int y, tfnsw_status_t status, bool has_realtime, int data_count)
{
    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_remove_style_all(dot);
    lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, 3, 0);

    uint32_t color;

    // Green: SUCCESS + realtime + actual data
    if (status == TFNSW_STATUS_SUCCESS && has_realtime && data_count > 0) {
        color = 0x00FF00;  // Green = live realtime data
    }
    // Yellow: SUCCESS but scheduled only (no realtime) or cached success
    else if ((status == TFNSW_STATUS_SUCCESS || status == TFNSW_STATUS_SUCCESS_CACHED) && data_count > 0) {
        color = 0xFFFF00;  // Yellow = scheduled only or cached
    }
    // Red: parse or network errors
    else if (status == TFNSW_STATUS_ERROR_PARSE ||
             status == TFNSW_STATUS_ERROR_NETWORK ||
             status == TFNSW_STATUS_ERROR_TIMEOUT ||
             status == TFNSW_STATUS_ERROR_SERVER) {
        color = 0xFF0000;  // Red = error
    }
    // Orange: fetching, idle, or other transitional states
    else {
        color = 0xFF8800;  // Orange = fetching/waiting
    }

    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
}

// Get color for delay/status
static uint32_t get_status_color(int mins, bool is_realtime, bool is_delayed)
{
    if (mins <= 0) {
        return 0x00FF00;  // Green for NOW
    } else if (is_delayed) {
        return 0xFF8800;  // Orange for delayed
    }
    return theme_accent_color;  // Theme color otherwise
}

// Render train car visualization with occupancy
static void render_train_cars(lv_obj_t* scr, int y, uint8_t num_cars, const tfnsw_departures_t* data)
{
    int train_w = 290;
    int train_h = 14;
    int train_x = (LCD_WIDTH - train_w) / 2;
    int car_w = (train_w - (num_cars - 1) * 2) / num_cars;
    int gap_w = 2;

    // Default loading levels if no data
    int default_loading[] = {70, 65, 80, 55, 45, 30, 25, 20, 25};

    for (int i = 0; i < num_cars && i < 9; i++) {
        int cx = train_x + i * (car_w + gap_w);

        lv_obj_t *car = lv_obj_create(scr);
        lv_obj_remove_style_all(car);
        lv_obj_set_scrollbar_mode(car, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(car, car_w, train_h);
        lv_obj_set_pos(car, cx, y);

        // Extract RGB from theme
        uint8_t r = (theme_accent_color >> 16) & 0xFF;
        uint8_t g = (theme_accent_color >> 8) & 0xFF;
        uint8_t b = theme_accent_color & 0xFF;

        // Get loading level (from data or default)
        int load = default_loading[i];
        if (data && data->count > 0 && data->departures[0].occupancy_percent > 0) {
            load = data->departures[0].occupancy_percent;
        }

        // Blend with dark gray based on loading
        uint8_t blend_r = (r * load + 0x2a * (100 - load)) / 100;
        uint8_t blend_g = (g * load + 0x2a * (100 - load)) / 100;
        uint8_t blend_b = (b * load + 0x2a * (100 - load)) / 100;
        uint32_t car_color = (blend_r << 16) | (blend_g << 8) | blend_b;

        lv_obj_set_style_bg_color(car, lv_color_hex(car_color), 0);
        lv_obj_set_style_bg_opa(car, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(car, lv_color_hex(theme_accent_color), 0);
        lv_obj_set_style_border_width(car, 1, 0);
        lv_obj_set_style_border_opa(car, LV_OPA_50, 0);
        lv_obj_set_style_border_side(car, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_radius(car, 2, 0);
    }

    // Front nose pieces
    lv_obj_t *nose_front1 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_front1);
    lv_obj_set_scrollbar_mode(nose_front1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_front1, 12, train_h - 2);
    lv_obj_set_pos(nose_front1, train_x - 10, y + 1);
    lv_obj_set_style_bg_color(nose_front1, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front1, 2, 0);

    lv_obj_t *nose_front2 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_front2);
    lv_obj_set_scrollbar_mode(nose_front2, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_front2, 8, train_h - 6);
    lv_obj_set_pos(nose_front2, train_x - 16, y + 3);
    lv_obj_set_style_bg_color(nose_front2, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front2, 2, 0);

    lv_obj_t *nose_front3 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_front3);
    lv_obj_set_scrollbar_mode(nose_front3, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_front3, 6, train_h - 10);
    lv_obj_set_pos(nose_front3, train_x - 20, y + 5);
    lv_obj_set_style_bg_color(nose_front3, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front3, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front3, 2, 0);

    // Rear nose pieces
    lv_obj_t *nose_rear1 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_rear1);
    lv_obj_set_scrollbar_mode(nose_rear1, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_rear1, 12, train_h - 2);
    lv_obj_set_pos(nose_rear1, train_x + train_w - 2, y + 1);
    lv_obj_set_style_bg_color(nose_rear1, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear1, 2, 0);

    lv_obj_t *nose_rear2 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_rear2);
    lv_obj_set_scrollbar_mode(nose_rear2, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_rear2, 8, train_h - 6);
    lv_obj_set_pos(nose_rear2, train_x + train_w + 8, y + 3);
    lv_obj_set_style_bg_color(nose_rear2, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear2, 2, 0);

    lv_obj_t *nose_rear3 = lv_obj_create(scr);
    lv_obj_remove_style_all(nose_rear3);
    lv_obj_set_scrollbar_mode(nose_rear3, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose_rear3, 6, train_h - 10);
    lv_obj_set_pos(nose_rear3, train_x + train_w + 14, y + 5);
    lv_obj_set_style_bg_color(nose_rear3, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear3, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear3, 2, 0);
}

// Forward declaration for recalc_minutes_until (defined later)
static int recalc_minutes_until(const tfnsw_departure_t* dep);

// Render a service row (destination + time)
static void render_service_row(lv_obj_t* scr, int y, const tfnsw_departure_t* dep, bool show_rt_dot, int font_size)
{
    const lv_font_t* font = (font_size >= 20) ? &lv_font_montserrat_20 :
                            (font_size >= 16) ? &lv_font_montserrat_16 : &lv_font_montserrat_14;

    // Recalculate minutes based on current time
    int mins_until = recalc_minutes_until(dep);

    // Realtime dot indicator
    if (show_rt_dot && dep->is_realtime) {
        lv_obj_t *rt_dot = lv_obj_create(scr);
        lv_obj_remove_style_all(rt_dot);
        lv_obj_set_scrollbar_mode(rt_dot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(rt_dot, 4, 4);
        lv_obj_set_pos(rt_dot, 8, y + (font_size / 2) - 2);
        lv_obj_set_style_bg_color(rt_dot, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_bg_opa(rt_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rt_dot, 2, 0);
    }

    // Destination
    lv_obj_t *dest = lv_label_create(scr);
    lv_label_set_text(dest, dep->destination[0] ? dep->destination : "Unknown");
    lv_obj_set_style_text_font(dest, font, 0);
    lv_obj_set_style_text_color(dest, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(dest, LCD_WIDTH - 80);
    lv_label_set_long_mode(dest, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(dest, show_rt_dot ? 16 : 10, y);

    // Minutes - use recalculated time
    char mins_str[16];
    if (mins_until <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else if (mins_until == 1) {
        snprintf(mins_str, sizeof(mins_str), "1 min");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%d min", mins_until);
    }

    lv_obj_t *mins = lv_label_create(scr);
    lv_label_set_text(mins, mins_str);
    lv_obj_set_style_text_font(mins, font, 0);
    lv_obj_set_style_text_color(mins, lv_color_hex(get_status_color(mins_until, dep->is_realtime, dep->is_delayed)), 0);
    lv_obj_align(mins, LV_ALIGN_TOP_RIGHT, -10, y);
}

// ============================================================================
// Unified View Renderer
// ============================================================================

// Recalculate minutes until departure based on current time
// Uses estimated_time if available (realtime), otherwise scheduled_time
static int recalc_minutes_until(const tfnsw_departure_t* dep)
{
    if (!dep) return 0;

    int64_t departure_time = dep->is_realtime && dep->estimated_time > 0
                             ? dep->estimated_time
                             : dep->scheduled_time;

    if (departure_time <= 0) {
        return dep->mins_to_departure;  // Fallback to stored value
    }

    // Get current time
    time_t now = time(NULL);
    int diff_seconds = (int)(departure_time - now);
    return diff_seconds / 60;
}

static void lcd_render_departure_view(const view_config_t* config, const tfnsw_departures_t* data)
{
    // Clean up previous timer
    if (view_rotation_timer) {
        lv_timer_del(view_rotation_timer);
        view_rotation_timer = NULL;
    }
    view_header_label = NULL;
    view_time_label = NULL;
    view_show_alt_text = false;
    current_render_config = config;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);

    const view_display_opts_t* opts = &config->display;
    bool is_realtime_source = (config->data_source == VIEW_DATA_REALTIME);

    // ===== HEADER BAR =====
    render_header(scr, config, is_realtime_source && opts->show_realtime_dot);

    // Realtime status dot - only show green when data is valid and has realtime info
    if (opts->show_realtime_dot && data) {
        bool has_rt = false;
        for (int i = 0; i < data->count && i < 3; i++) {
            if (data->departures[i].is_realtime) {
                has_rt = true;
                break;
            }
        }
        render_status_dot(scr, LCD_WIDTH - 70, 9, data->status, has_rt, data->count);
    }

    int y_pos = 26;

    // ===== DIRECTION INDICATOR =====
    if (opts->show_direction_arrow && config->direction_text[0]) {
        lv_obj_t *dir_label = lv_label_create(scr);
        char dir_str[32];
        const char* arrow = (config->direction == TFNSW_DIRECTION_NORTHBOUND) ? "^" : "v";
        snprintf(dir_str, sizeof(dir_str), "%s %s", arrow, config->direction_text);
        lv_label_set_text(dir_label, dir_str);
        lv_obj_set_style_text_font(dir_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(dir_label, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_set_pos(dir_label, 8, y_pos);
        y_pos += 16;
    }

    // Handle no data - check various conditions
    if (!data || data->count == 0) {
        lv_obj_t *no_svc = lv_label_create(scr);
        const char* msg = "No services";

        // Check for specific error conditions
        if (data && data->error_message[0]) {
            msg = data->error_message;
        } else if (!tfnsw_has_api_key()) {
            msg = "API key required";
        } else if (tfnsw_is_fetching()) {
            msg = "Fetching data...";
        } else if (!data || data->status == TFNSW_STATUS_IDLE) {
            msg = "Waiting for data...";
        } else if (data->status == TFNSW_STATUS_FETCHING) {
            msg = "Fetching data...";
        } else if (data->status == TFNSW_STATUS_ERROR_NO_API_KEY) {
            msg = "API key required";
        } else if (data->status == TFNSW_STATUS_ERROR_NETWORK) {
            msg = "Network error";
        } else if (data->status == TFNSW_STATUS_ERROR_TIMEOUT) {
            msg = "Request timeout";
        } else if (data->status == TFNSW_STATUS_ERROR_AUTH) {
            msg = "Invalid API key";
        } else if (data->status == TFNSW_STATUS_ERROR_PARSE) {
            msg = "Data parse error";
        } else if (data->status == TFNSW_STATUS_ERROR_NO_DATA) {
            msg = "No services found";
        }

        ESP_LOGI("LCD", "No data to display: %s (status=%d, api_key=%d, fetching=%d)",
                 msg, data ? data->status : -1, tfnsw_has_api_key(), tfnsw_is_fetching());

        lv_label_set_text(no_svc, msg);
        lv_obj_set_style_text_font(no_svc, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(no_svc, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(no_svc, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // ===== MAIN DEPARTURE =====
    const tfnsw_departure_t* first = &data->departures[0];
    int first_mins = recalc_minutes_until(first);  // Recalculate based on current time

    // Destination (large)
    lv_obj_t *dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, first->destination[0] ? first->destination : "Unknown");
    lv_obj_set_style_text_font(dest_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dest_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(dest_lbl, LCD_WIDTH - 90);
    lv_label_set_long_mode(dest_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(dest_lbl, 10, y_pos);

    // Minutes (large) - use recalculated time
    char mins_str[16];
    if (first_mins <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else if (first_mins == 1) {
        snprintf(mins_str, sizeof(mins_str), "1min");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%dmin", first_mins);
    }

    view_time_label = lv_label_create(scr);
    lv_label_set_text(view_time_label, mins_str);
    lv_obj_set_style_text_font(view_time_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(view_time_label, lv_color_hex(get_status_color(first_mins, first->is_realtime, first->is_delayed)), 0);
    lv_obj_align(view_time_label, LV_ALIGN_TOP_RIGHT, -10, y_pos);

    y_pos += 26;

    // ===== DELAY STATUS =====
    if (opts->show_delay_status) {
        lv_obj_t *status_lbl = lv_label_create(scr);
        if (first->is_realtime) {
            if (first->delay_seconds > 60) {
                char delay_str[24];
                snprintf(delay_str, sizeof(delay_str), "+%dm late", first->delay_seconds / 60);
                lv_label_set_text(status_lbl, delay_str);
                lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xFF8800), 0);
            } else if (first->delay_seconds < -60) {
                lv_label_set_text(status_lbl, "Early");
                lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00AAFF), 0);
            } else {
                lv_label_set_text(status_lbl, "LIVE - On time");
                lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00FF00), 0);
            }
        } else {
            lv_label_set_text(status_lbl, "Scheduled");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(THEME_SECONDARY), 0);
        }
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(status_lbl, 10, y_pos);
        y_pos += 16;
    }

    // ===== CALLING STATIONS (scrolling) =====
    if (opts->show_calling_stations && first->calling_stations[0]) {
        lv_obj_t *calling_lbl = lv_label_create(scr);
        lv_label_set_text(calling_lbl, first->calling_stations);
        lv_obj_set_style_text_font(calling_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(calling_lbl, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(calling_lbl, LCD_WIDTH - 20);
        lv_label_set_long_mode(calling_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_speed(calling_lbl, 25, 0);
        lv_obj_set_pos(calling_lbl, 10, y_pos);
        y_pos += 22;
    }

    // ===== TRAIN CARS =====
    if (opts->show_train_cars && opts->train_car_count > 0) {
        render_train_cars(scr, y_pos, opts->train_car_count, data);
        y_pos += 20;
    }

    // ===== SEPARATOR =====
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_remove_style_all(sep);
    lv_obj_set_scrollbar_mode(sep, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(sep, LCD_WIDTH - 16, 1);
    lv_obj_set_pos(sep, 8, y_pos);
    lv_obj_set_style_bg_color(sep, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    y_pos += 6;

    // ===== FOLLOWING SERVICES =====
    int row_height = opts->show_train_cars ? 24 : 22;
    int font_size = opts->show_train_cars ? 16 : 14;

    for (int i = 1; i < data->count && i <= opts->max_following; i++) {
        render_service_row(scr, y_pos, &data->departures[i], opts->show_realtime_dot, font_size);
        y_pos += row_height;
    }

    // ===== BOTTOM STATUS (errors) =====
    if (data->status != TFNSW_STATUS_SUCCESS && data->error_message[0]) {
        lv_obj_t *err = lv_label_create(scr);
        lv_label_set_text(err, data->error_message);
        lv_obj_set_style_text_font(err, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(err, lv_color_hex(0xFF8800), 0);
        lv_obj_align(err, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    // Start rotation timer if needed
    if (opts->rotate_header_text && config->alt_header[0]) {
        view_rotation_timer = lv_timer_create(view_rotation_timer_cb, 3000, NULL);
    }
}

// ============================================================================
// LVGL Display Flush Callback
// ============================================================================
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;

    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

// ============================================================================
// LCD Initialization
// ============================================================================
esp_err_t lcd_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing LCD with LVGL...");
    ESP_LOGI(TAG, "LCD pins: MOSI=%d, SCLK=%d, CS=%d, DC=%d, RST=%d, BL=%d",
             LCD_PIN_MOSI, LCD_PIN_SCLK, LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);

    // Configure backlight PWM
    ESP_LOGI(TAG, "Configuring backlight PWM...");
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_PIN_BL,
        .duty = 51,  // 20% brightness (51/255)
        .hpoint = 0
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Backlight configured");

    // Configure SPI bus for LCD only (SD card disabled to avoid conflicts)
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,  // LCD doesn't need MISO
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t)
    };
    ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SPI bus initialized");

    // Configure LCD panel IO
    ESP_LOGI(TAG, "Configuring LCD panel IO...");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LCD panel IO: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LCD panel IO configured");

    // Configure ST7789 panel
    ESP_LOGI(TAG, "Creating ST7789 panel...");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize panel
    ESP_LOGI(TAG, "Resetting LCD panel...");
    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset LCD panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Initializing LCD panel...");
    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LCD panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_invert_color(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to invert colors: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure for landscape orientation (320x172)
    // ST7789 172x320: gap is 34 pixels because controller is 240x320
    ESP_LOGI(TAG, "Configuring display orientation...");
    ret = esp_lcd_panel_set_gap(panel_handle, 0, 34);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set gap: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_swap_xy(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to swap xy: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_mirror(panel_handle, true, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mirror: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn display on: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LCD panel initialized: %dx%d", LCD_WIDTH, LCD_HEIGHT);

    // Initialize LVGL
    lv_init();

    // Allocate LVGL draw buffers
    buf1 = heap_caps_malloc(LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffers");
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LVGL_BUF_SIZE);

    // Initialize display driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = panel_handle;
    disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "LVGL initialized successfully");
    return ESP_OK;
}

void lcd_set_backlight(uint8_t brightness)
{
    uint32_t duty = (brightness * 255) / 100;
    ESP_LOGI(TAG, "Setting backlight: %d%% (duty: %lu/255)", brightness, duty);
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_set_duty failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_update_duty failed: %s", esp_err_to_name(ret));
    }
}

// Track last refresh time for periodic updates
static uint32_t last_realtime_refresh_ms = 0;
#define REALTIME_REFRESH_INTERVAL_MS 30000  // Refresh realtime views every 30 seconds

void lcd_update(void)
{
    // Periodic refresh for realtime views (ensures countdown stays accurate)
    const view_config_t* curr_config = lcd_get_view_config(current_view);
    if (curr_config && curr_config->data_source == VIEW_DATA_REALTIME) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_realtime_refresh_ms >= REALTIME_REFRESH_INTERVAL_MS) {
            last_realtime_refresh_ms = now_ms;
            lcd_refresh_scene();
            ESP_LOGI("LCD", "Periodic refresh for realtime view %d", current_view);
        }
    }

    // Process pending scene/view change (thread-safe: only modify LVGL from main loop)
    if (pending_scene >= 0) {
        view_id_t old_view = current_view;
        current_view = (view_id_t)pending_scene;
        current_scene = (lcd_scene_t)pending_scene;  // Keep legacy scene in sync
        pending_scene = -1;

        // Clean up rotation timers when leaving views
        if (old_view != current_view) {
            if (view_rotation_timer) {
                lv_timer_del(view_rotation_timer);
                view_rotation_timer = NULL;
            }
            if (hs_rotation_timer) {
                lv_timer_del(hs_rotation_timer);
                hs_rotation_timer = NULL;
            }
            view_header_label = NULL;
            view_time_label = NULL;
            hs_header_label = NULL;
            hs_time_label = NULL;
        }

        // Apply view config color and LED
        const view_config_t* config = lcd_get_view_config(current_view);
        if (config) {
            theme_accent_color = config->accent_color;
            if (current_view == VIEW_STATUS_INFO) {
                rgb_led_set_status(rgb_led_get_status());  // Re-enable status mode
            } else {
                rgb_led_set_hex(config->led_color);
            }

            // Reset periodic refresh timer when switching to realtime view
            if (config->data_source == VIEW_DATA_REALTIME) {
                last_realtime_refresh_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }

        lcd_refresh_scene();
    }

    // Process pending theme change
    if (theme_change_pending) {
        theme_accent_color = pending_theme;
        theme_change_pending = false;
        lcd_refresh_scene();
    }

    // Process pending view data updates
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (view_data_pending[i]) {
            view_data_pending[i] = false;
            if (current_view == (view_id_t)i) {
                lcd_refresh_scene();
            }
        }
    }

    // Process pending realtime update (legacy)
    if (pending_realtime_update) {
        pending_realtime_update = false;
        lcd_apply_realtime_update();
    }

    // Process pending dual-direction update (legacy)
    if (pending_dual_update) {
        pending_dual_update = false;
        lcd_apply_dual_update();
    }

    // Process pending simple mode updates (legacy)
    if (pending_north_update) {
        pending_north_update = false;
        lcd_apply_simple_update(true);
    }
    if (pending_south_update) {
        pending_south_update = false;
        lcd_apply_simple_update(false);
    }

    lv_timer_handler();
}

// ============================================================================
// Scene Management
// ============================================================================

lcd_scene_t lcd_get_current_scene(void)
{
    return current_scene;
}

void lcd_set_scene(lcd_scene_t scene)
{
    if (scene < SCENE_COUNT) {
        // Set pending scene - will be applied in lcd_update() from main loop
        pending_scene = (int)scene;
    }
}

void lcd_next_scene(void)
{
    // Calculate next scene and set as pending - will be applied in lcd_update() from main loop
    // This is thread-safe: LVGL operations only happen in the main loop
    pending_scene = (current_scene + 1) % SCENE_COUNT;
}

void lcd_refresh_scene(void)
{
    // Use the unified view system
    lcd_render_current_view();
}

// ============================================================================
// Screen Templates using LVGL
// ============================================================================

void lcd_show_splash(void)
{
    // Set brightness to 80% for splash
    lcd_set_backlight(80);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Silver Emu");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    // Subtitle
    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Service Co.");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 62);

    // ===== ANIMATED TRAIN =====
    // Create a container for the train that will animate
    lv_obj_t *train_container = lv_obj_create(scr);
    lv_obj_remove_style_all(train_container);
    lv_obj_set_scrollbar_mode(train_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(train_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(train_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(train_container, 0, 0);
    lv_obj_set_size(train_container, 120, 16);
    lv_obj_set_pos(train_container, -120, 100);  // Start off-screen left

    // Train cars (5 cars)
    int car_w = 20;
    int car_h = 12;
    int gap = 2;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *car = lv_obj_create(train_container);
        lv_obj_remove_style_all(car);
        lv_obj_set_scrollbar_mode(car, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(car, car_w, car_h);
        lv_obj_set_pos(car, i * (car_w + gap) + 10, 2);
        lv_obj_set_style_bg_color(car, lv_color_hex(theme_accent_color), 0);
        lv_obj_set_style_bg_opa(car, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(car, 2, 0);
    }

    // Front nose (triangle effect)
    lv_obj_t *nose = lv_obj_create(train_container);
    lv_obj_remove_style_all(nose);
    lv_obj_set_scrollbar_mode(nose, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(nose, 8, 12);
    lv_obj_set_pos(nose, 0, 2);
    lv_obj_set_style_bg_color(nose, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose, 2, 0);

    // Animate train moving from left to right
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, train_container);
    lv_anim_set_values(&anim, -120, LCD_WIDTH + 20);
    lv_anim_set_time(&anim, 2200);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);

    // Brand
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text(ver, "by Turnout Labs");
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// Data setter functions
void lcd_set_ip(const char* ip)
{
    if (ip) {
        strncpy(current_ip, ip, sizeof(current_ip) - 1);
        current_ip[sizeof(current_ip) - 1] = '\0';
    }
}

void lcd_set_wifi_ssid(const char* ssid)
{
    if (ssid) {
        strncpy(current_ssid, ssid, sizeof(current_ssid) - 1);
        current_ssid[sizeof(current_ssid) - 1] = '\0';
    }
}

void lcd_set_wifi_rssi(int rssi)
{
    current_rssi = rssi;
}

void lcd_set_uptime(uint32_t seconds)
{
    current_uptime = seconds;
}

// Departure board setter functions
void lcd_set_departure_destination(const char* destination)
{
    if (destination) {
        strncpy(departure_destination, destination, sizeof(departure_destination) - 1);
        departure_destination[sizeof(departure_destination) - 1] = '\0';
    }
}

void lcd_set_departure_calling(const char* calling_stations)
{
    if (calling_stations) {
        strncpy(departure_calling, calling_stations, sizeof(departure_calling) - 1);
        departure_calling[sizeof(departure_calling) - 1] = '\0';
    }
}

void lcd_set_departure_time(const char* time)
{
    if (time) {
        strncpy(departure_time, time, sizeof(departure_time) - 1);
        departure_time[sizeof(departure_time) - 1] = '\0';
    }
}

void lcd_set_departure_mins(int mins)
{
    departure_mins = mins;
}

void lcd_set_next_departure(const char* next_time, const char* next_dest)
{
    if (next_time) {
        strncpy(next_departure_time, next_time, sizeof(next_departure_time) - 1);
        next_departure_time[sizeof(next_departure_time) - 1] = '\0';
    }
    if (next_dest) {
        strncpy(next_departure_dest, next_dest, sizeof(next_departure_dest) - 1);
        next_departure_dest[sizeof(next_departure_dest) - 1] = '\0';
    }
}

void lcd_set_next2_departure(const char* next_time, const char* next_dest)
{
    if (next_time) {
        strncpy(next2_departure_time, next_time, sizeof(next2_departure_time) - 1);
        next2_departure_time[sizeof(next2_departure_time) - 1] = '\0';
    }
    if (next_dest) {
        strncpy(next2_departure_dest, next_dest, sizeof(next2_departure_dest) - 1);
        next2_departure_dest[sizeof(next2_departure_dest) - 1] = '\0';
    }
}

void lcd_show_departure_board(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ===== HEADER BAR (simple label with bg) =====
    lv_obj_t *header_bg = lv_label_create(scr);
    lv_obj_set_size(header_bg, LCD_WIDTH, 26);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    lv_obj_t *next_svc = lv_label_create(scr);
    lv_label_set_text(next_svc, "Metro");
    lv_obj_set_style_text_font(next_svc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(next_svc, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(next_svc, 8, 5);

    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 5);

    // ===== DESTINATION + TIME =====
    lv_obj_t *dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, departure_destination);
    lv_obj_set_style_text_font(dest_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dest_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(dest_lbl, 10, 30);

    char mins_str[16];
    if (departure_mins <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%d min", departure_mins);
    }
    lv_obj_t *mins_lbl = lv_label_create(scr);
    lv_label_set_text(mins_lbl, mins_str);
    lv_obj_set_style_text_font(mins_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mins_lbl, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(mins_lbl, LV_ALIGN_TOP_RIGHT, -10, 30);

    // ===== CALLING STATIONS (slow scroll) =====
    lv_obj_t *calling_lbl = lv_label_create(scr);
    lv_label_set_text(calling_lbl, departure_calling);
    lv_obj_set_style_text_font(calling_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(calling_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(calling_lbl, LCD_WIDTH - 20);
    lv_label_set_long_mode(calling_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(calling_lbl, 20, 0);  // Slow scroll
    lv_obj_set_pos(calling_lbl, 10, 58);

    // ===== SEPARATOR (simple label with bg) =====
    lv_obj_t *line = lv_label_create(scr);
    lv_obj_set_size(line, LCD_WIDTH, 1);
    lv_obj_set_pos(line, 0, 80);
    lv_obj_set_style_bg_color(line, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    // ===== NEXT SERVICES =====
    lv_obj_t *next_dest = lv_label_create(scr);
    lv_label_set_text(next_dest, next_departure_dest);
    lv_obj_set_style_text_font(next_dest, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next_dest, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(next_dest, 10, 90);

    lv_obj_t *next_mins = lv_label_create(scr);
    lv_label_set_text(next_mins, next_departure_time);
    lv_obj_set_style_text_font(next_mins, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next_mins, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(next_mins, LV_ALIGN_TOP_RIGHT, -10, 90);

    lv_obj_t *next2_dest = lv_label_create(scr);
    lv_label_set_text(next2_dest, next2_departure_dest);
    lv_obj_set_style_text_font(next2_dest, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next2_dest, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(next2_dest, 10, 115);

    lv_obj_t *next2_mins = lv_label_create(scr);
    lv_label_set_text(next2_mins, next2_departure_time);
    lv_obj_set_style_text_font(next2_mins, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next2_mins, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(next2_mins, LV_ALIGN_TOP_RIGHT, -10, 115);

    lv_obj_t *next3_dest = lv_label_create(scr);
    lv_label_set_text(next3_dest, "Tallawong");
    lv_obj_set_style_text_font(next3_dest, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next3_dest, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(next3_dest, 10, 140);

    lv_obj_t *next3_mins = lv_label_create(scr);
    lv_label_set_text(next3_mins, "14 min");
    lv_obj_set_style_text_font(next3_mins, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(next3_mins, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(next3_mins, LV_ALIGN_TOP_RIGHT, -10, 140);
}

// High speed rotation timer callback - toggles header text
static void hs_rotation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    hs_show_alt_text = !hs_show_alt_text;

    if (hs_header_label && lv_obj_is_valid(hs_header_label)) {
        lv_label_set_text(hs_header_label, hs_show_alt_text ? "Go to platform" : "High Speed");
    }

    // Use first service (services are pre-sorted by departure time)
    const hs_service_t* svc = &hs_services[0];
    static char mins_str[16];
    snprintf(mins_str, sizeof(mins_str), "%d min", svc->mins_to_departure);

    if (hs_time_label && lv_obj_is_valid(hs_time_label)) {
        lv_label_set_text(hs_time_label, hs_show_alt_text ? "Plat. A" : mins_str);
    }
}

void lcd_show_high_speed(void)
{
    // Clean up previous timer if exists
    if (hs_rotation_timer) {
        lv_timer_del(hs_rotation_timer);
        hs_rotation_timer = NULL;
    }
    hs_header_label = NULL;
    hs_time_label = NULL;
    hs_dest_label = NULL;
    hs_mins_label = NULL;
    hs_calling_label = NULL;
    hs_show_alt_text = false;

    // Use first service (services are pre-sorted by departure time)
    const hs_service_t* svc = &hs_services[0];

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ===== HEADER BAR =====
    lv_obj_t *header_bg = lv_obj_create(scr);
    lv_obj_remove_style_all(header_bg);
    lv_obj_set_scrollbar_mode(header_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(header_bg, LCD_WIDTH, 24);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    hs_header_label = lv_label_create(scr);
    lv_label_set_text(hs_header_label, "High Speed");
    lv_obj_set_style_text_font(hs_header_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hs_header_label, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(hs_header_label, 8, 4);

    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 4);

    // ===== DESTINATION + TIME =====
    hs_dest_label = lv_label_create(scr);
    lv_label_set_text(hs_dest_label, svc->destination);
    lv_obj_set_style_text_font(hs_dest_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hs_dest_label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(hs_dest_label, 10, 28);

    static char mins_str[16];
    snprintf(mins_str, sizeof(mins_str), "%d min", svc->mins_to_departure);
    hs_mins_label = lv_label_create(scr);
    lv_label_set_text(hs_mins_label, mins_str);
    lv_obj_set_style_text_font(hs_mins_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hs_mins_label, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(hs_mins_label, LV_ALIGN_TOP_RIGHT, -10, 28);

    // Also store reference for header time label (used by rotation timer)
    hs_time_label = hs_mins_label;

    // ===== CALLING STATIONS (scrolling with service info) =====
    hs_calling_label = lv_label_create(scr);
    static char initial_scroll[512];
    snprintf(initial_scroll, sizeof(initial_scroll), "Calling at: %s      %s      ",
        svc->calling_stations, svc->info_text);
    lv_label_set_text(hs_calling_label, initial_scroll);
    lv_obj_set_style_text_font(hs_calling_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hs_calling_label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(hs_calling_label, LCD_WIDTH - 20);
    lv_label_set_long_mode(hs_calling_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(hs_calling_label, 25, 0);
    lv_obj_set_pos(hs_calling_label, 10, 54);

    // ===== TRAIN SILHOUETTE (9 cars) =====
    int train_w = 290;
    int train_h = 14;
    int train_x = (LCD_WIDTH - train_w) / 2;
    int train_y = 78;
    int num_cars = 9;
    int car_w = (train_w - (num_cars - 1) * 2) / num_cars;
    int gap_w = 2;

    // Passenger loading levels (0-100, higher = more crowded)
    int loading[] = {70, 65, 80, 55, 45, 30, 25, 20, 25};

    for (int i = 0; i < num_cars; i++) {
        int cx = train_x + i * (car_w + gap_w);

        lv_obj_t *car = lv_label_create(scr);
        lv_label_set_text(car, "");
        lv_obj_set_size(car, car_w, train_h);
        lv_obj_set_pos(car, cx, train_y);

        uint8_t r = (theme_accent_color >> 16) & 0xFF;
        uint8_t g = (theme_accent_color >> 8) & 0xFF;
        uint8_t b = theme_accent_color & 0xFF;

        int load = loading[i];
        uint8_t blend_r = (r * load + 0x2a * (100 - load)) / 100;
        uint8_t blend_g = (g * load + 0x2a * (100 - load)) / 100;
        uint8_t blend_b = (b * load + 0x2a * (100 - load)) / 100;
        uint32_t car_color = (blend_r << 16) | (blend_g << 8) | blend_b;

        lv_obj_set_style_bg_color(car, lv_color_hex(car_color), 0);
        lv_obj_set_style_bg_opa(car, LV_OPA_COVER, 0);

        // Add top and bottom border in theme color at 50% opacity
        lv_obj_set_style_border_color(car, lv_color_hex(theme_accent_color), 0);
        lv_obj_set_style_border_width(car, 1, 0);
        lv_obj_set_style_border_opa(car, LV_OPA_50, 0);
        lv_obj_set_style_border_side(car, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);

        // All cars have tiny radius - nose pieces create pointed ends
        lv_obj_set_style_radius(car, 2, 0);
    }

    // Front nose - pointed triangle shape (layered rectangles) in theme color
    lv_obj_t *nose_front1 = lv_label_create(scr);
    lv_label_set_text(nose_front1, "");
    lv_obj_set_size(nose_front1, 12, train_h - 2);
    lv_obj_set_pos(nose_front1, train_x - 10, train_y + 1);
    lv_obj_set_style_bg_color(nose_front1, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front1, 2, 0);

    lv_obj_t *nose_front2 = lv_label_create(scr);
    lv_label_set_text(nose_front2, "");
    lv_obj_set_size(nose_front2, 8, train_h - 6);
    lv_obj_set_pos(nose_front2, train_x - 16, train_y + 3);
    lv_obj_set_style_bg_color(nose_front2, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front2, 2, 0);

    lv_obj_t *nose_front3 = lv_label_create(scr);
    lv_label_set_text(nose_front3, "");
    lv_obj_set_size(nose_front3, 6, train_h - 10);
    lv_obj_set_pos(nose_front3, train_x - 20, train_y + 5);
    lv_obj_set_style_bg_color(nose_front3, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_front3, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_front3, 2, 0);

    // Rear nose - pointed triangle shape (layered rectangles) in theme color
    lv_obj_t *nose_rear1 = lv_label_create(scr);
    lv_label_set_text(nose_rear1, "");
    lv_obj_set_size(nose_rear1, 12, train_h - 2);
    lv_obj_set_pos(nose_rear1, train_x + train_w - 2, train_y + 1);
    lv_obj_set_style_bg_color(nose_rear1, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear1, 2, 0);

    lv_obj_t *nose_rear2 = lv_label_create(scr);
    lv_label_set_text(nose_rear2, "");
    lv_obj_set_size(nose_rear2, 8, train_h - 6);
    lv_obj_set_pos(nose_rear2, train_x + train_w + 8, train_y + 3);
    lv_obj_set_style_bg_color(nose_rear2, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear2, 2, 0);

    lv_obj_t *nose_rear3 = lv_label_create(scr);
    lv_label_set_text(nose_rear3, "");
    lv_obj_set_size(nose_rear3, 6, train_h - 10);
    lv_obj_set_pos(nose_rear3, train_x + train_w + 14, train_y + 5);
    lv_obj_set_style_bg_color(nose_rear3, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(nose_rear3, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nose_rear3, 2, 0);

    // ===== NEXT SERVICES (show services 1-3 in departure time order) =====
    int next_y[] = {106, 130, 154};
    for (int i = 1; i < HS_SERVICE_COUNT && i <= 3; i++) {
        // Use sequential index for time-ordered display
        const hs_service_t* next_svc = &hs_services[i];

        lv_obj_t *next_dest = lv_label_create(scr);
        lv_label_set_text(next_dest, next_svc->destination);
        lv_obj_set_style_text_font(next_dest, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(next_dest, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_pos(next_dest, 10, next_y[i-1]);

        static char next_mins_str[4][16];  // Static buffer for each row
        snprintf(next_mins_str[i], sizeof(next_mins_str[i]), "%d min", next_svc->mins_to_departure);
        lv_obj_t *next_mins = lv_label_create(scr);
        lv_label_set_text(next_mins, next_mins_str[i]);
        lv_obj_set_style_text_font(next_mins, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(next_mins, lv_color_hex(theme_accent_color), 0);
        lv_obj_align(next_mins, LV_ALIGN_TOP_RIGHT, -10, next_y[i-1]);
    }

    // Start rotation timer (toggle every 3 seconds - cycles header/platform)
    hs_rotation_timer = lv_timer_create(hs_rotation_timer_cb, 3000, NULL);

    // No service cycling timer - destination stays fixed once view is selected
}

void lcd_show_status_info(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header = lv_label_create(scr);
    lv_label_set_text(header, "STATUS");
    lv_obj_set_style_text_font(header, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 8);

    // IP Address
    lv_obj_t *ip_label = lv_label_create(scr);
    lv_label_set_text(ip_label, "IP Address");
    lv_obj_set_style_text_font(ip_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ip_label, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_pos(ip_label, 15, 35);

    lv_obj_t *ip_value = lv_label_create(scr);
    lv_label_set_text(ip_value, current_ip);
    lv_obj_set_style_text_font(ip_value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ip_value, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(ip_value, 15, 50);

    // Network
    lv_obj_t *ssid_label = lv_label_create(scr);
    lv_label_set_text(ssid_label, "Network");
    lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_pos(ssid_label, 15, 80);

    lv_obj_t *ssid_value = lv_label_create(scr);
    lv_label_set_text(ssid_value, current_ssid[0] ? current_ssid : "--");
    lv_obj_set_style_text_font(ssid_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ssid_value, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(ssid_value, 15, 95);

    // Signal
    lv_obj_t *rssi_label = lv_label_create(scr);
    lv_label_set_text(rssi_label, "Signal");
    lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rssi_label, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_pos(rssi_label, 15, 120);

    char rssi_str[16];
    snprintf(rssi_str, sizeof(rssi_str), "%d dBm", current_rssi);
    lv_obj_t *rssi_value = lv_label_create(scr);
    lv_label_set_text(rssi_value, current_rssi != 0 ? rssi_str : "--");
    lv_obj_set_style_text_font(rssi_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(rssi_value, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_pos(rssi_value, 15, 135);

    // Version at bottom
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text(ver, "v" FIRMWARE_VERSION);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -5);
}

void lcd_show_wifi_config(const char* ssid, const char* ip)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(header, LCD_WIDTH, 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    lv_obj_t *header_txt = lv_label_create(header);
    lv_label_set_text(header_txt, "WiFi Setup");
    lv_obj_set_style_text_font(header_txt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header_txt, lv_color_hex(THEME_BG), 0);
    lv_obj_center(header_txt);

    // Connect to
    lv_obj_t *lbl1 = lv_label_create(scr);
    lv_label_set_text(lbl1, "Connect to WiFi:");
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl1, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(lbl1, LV_ALIGN_TOP_MID, 0, 38);

    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, ssid);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 55);

    // Password
    lv_obj_t *lbl2 = lv_label_create(scr);
    lv_label_set_text(lbl2, "Password:");
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl2, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(lbl2, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *pass_lbl = lv_label_create(scr);
    lv_label_set_text(pass_lbl, WIFI_AP_PASS);
    lv_obj_set_style_text_font(pass_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pass_lbl, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_MID, 0, 97);

    // IP Address - Large
    lv_obj_t *lbl3 = lv_label_create(scr);
    lv_label_set_text(lbl3, "Then open:");
    lv_obj_set_style_text_font(lbl3, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl3, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(lbl3, LV_ALIGN_TOP_MID, 0, 122);

    lv_obj_t *ip_lbl = lv_label_create(scr);
    lv_label_set_text(ip_lbl, ip);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(ip_lbl, LV_ALIGN_TOP_MID, 0, 138);

    lv_refr_now(NULL);
}

void lcd_show_connecting(const char* ssid)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Connecting text
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connecting...");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // SSID
    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, ssid);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 60);

    // Spinner
    lv_obj_t *spinner = lv_spinner_create(scr, 1000, 60);
    lv_obj_set_size(spinner, 50, 50);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(THEME_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(theme_accent_color), LV_PART_INDICATOR);

    lv_refr_now(NULL);
}

void lcd_show_connected(const char* ssid, const char* ip)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Checkmark indicator
    lv_obj_t *led = lv_led_create(scr);
    lv_obj_set_size(led, 30, 30);
    lv_obj_align(led, LV_ALIGN_TOP_MID, 0, 15);
    lv_led_set_color(led, lv_color_hex(theme_accent_color));
    lv_led_on(led);

    // Connected text
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Connected!");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    // SSID
    lv_obj_t *ssid_lbl = lv_label_create(scr);
    lv_label_set_text(ssid_lbl, ssid);
    lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 78);

    // IP Address - Large
    lv_obj_t *ip_bg = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(ip_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(ip_bg, LCD_WIDTH - 40, 45);
    lv_obj_align(ip_bg, LV_ALIGN_CENTER, 0, 25);
    lv_obj_set_style_bg_color(ip_bg, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_radius(ip_bg, 5, 0);
    lv_obj_set_style_border_width(ip_bg, 0, 0);

    lv_obj_t *ip_lbl = lv_label_create(ip_bg);
    lv_label_set_text(ip_lbl, ip);
    lv_obj_set_style_text_font(ip_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ip_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_center(ip_lbl);

    // Footer
    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, "Open in browser");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_refr_now(NULL);
}

void lcd_show_error(const char* message)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header with warning color (keep red for errors to stand out)
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(header, LCD_WIDTH, 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    lv_obj_t *header_txt = lv_label_create(header);
    lv_label_set_text(header_txt, "Error");
    lv_obj_set_style_text_font(header_txt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(header_txt, lv_color_hex(THEME_TEXT), 0);
    lv_obj_center(header_txt);

    // Error message
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, message);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(msg, LCD_WIDTH - 20);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    // Footer
    lv_obj_t *footer = lv_label_create(scr);
    lv_label_set_text(footer, "Press reset to retry");
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_refr_now(NULL);
}

// Legacy functions for compatibility
void lcd_clear(uint16_t color)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(color), 0);
    lv_refr_now(NULL);
}

void lcd_draw_string(int x, int y, const char* str, uint16_t color, uint16_t bg, uint8_t size)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, str);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);

    const lv_font_t *font = &lv_font_montserrat_14;
    if (size >= 4) font = &lv_font_montserrat_32;
    else if (size >= 3) font = &lv_font_montserrat_24;
    else if (size >= 2) font = &lv_font_montserrat_16;
    else font = &lv_font_montserrat_12;

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, x, y);
}

void lcd_draw_string_centered(int y, const char* str, uint16_t color, uint16_t bg, uint8_t size)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, str);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);

    const lv_font_t *font = &lv_font_montserrat_14;
    if (size >= 4) font = &lv_font_montserrat_32;
    else if (size >= 3) font = &lv_font_montserrat_24;
    else if (size >= 2) font = &lv_font_montserrat_16;
    else font = &lv_font_montserrat_12;

    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    lv_obj_t *rect = lv_obj_create(lv_scr_act());
    lv_obj_set_scrollbar_mode(rect, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_color(rect, lv_color_hex(color), 0);
    lv_obj_set_style_radius(rect, 0, 0);
    lv_obj_set_style_border_width(rect, 0, 0);
}

void lcd_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    lv_obj_t *rect = lv_obj_create(lv_scr_act());
    lv_obj_set_scrollbar_mode(rect, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_opa(rect, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(rect, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(rect, 1, 0);
    lv_obj_set_style_radius(rect, 0, 0);
}

// ============================================================================
// Theme Management
// ============================================================================

void lcd_set_theme_accent(uint32_t color)
{
    // Set pending theme - will be applied in lcd_update() from main loop
    pending_theme = color;
    theme_change_pending = true;
}

uint32_t lcd_get_theme_accent(void)
{
    return theme_accent_color;
}

// ============================================================================
// Realtime Data Display Functions
// ============================================================================

static const char* get_current_time_str(void)
{
    static char time_str[16];
    time_t now = time(NULL);
    struct tm* tm_now = localtime(&now);
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm_now->tm_hour, tm_now->tm_min);
    return time_str;
}

void lcd_set_display_status(display_status_t status)
{
    current_display_status = status;
}

display_status_t lcd_get_display_status(void)
{
    return current_display_status;
}

void lcd_set_realtime_indicator(bool is_realtime)
{
    is_realtime_data = is_realtime;
}

void lcd_set_delay_indicator(int delay_seconds)
{
    current_delay_seconds = delay_seconds;
}

void lcd_show_api_key_required(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Warning icon placeholder
    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, "!");
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 20);

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "API Key Required");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    // Instructions
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Get your free API key from\nopendata.transport.nsw.gov.au");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, LCD_WIDTH - 20);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 95);

    // Setup hint
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Configure via web interface");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    // IP address
    lv_obj_t *ip = lv_label_create(scr);
    lv_label_set_text(ip, current_ip);
    lv_obj_set_style_text_font(ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ip, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(ip, LV_ALIGN_BOTTOM_MID, 0, -5);

    current_display_status = DISPLAY_STATUS_NO_API_KEY;
}

void lcd_show_fetching(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Spinner
    lv_obj_t *spinner = lv_spinner_create(scr, 1000, 60);
    lv_obj_set_size(spinner, 50, 50);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(THEME_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(theme_accent_color), LV_PART_INDICATOR);

    // Text
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Fetching departures...");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 40);

    current_display_status = DISPLAY_STATUS_FETCHING;
}

// Sine wave animation callback
static void sine_wave_anim_cb(void *var, int32_t v)
{
    lv_obj_t *dot = (lv_obj_t *)var;
    if (dot && lv_obj_is_valid(dot)) {
        lv_obj_set_y(dot, 86 + (v * 15 / 100));  // Center Y with sine offset
    }
}

void lcd_show_loading(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Create animated sine wave dots
    int num_dots = 5;
    int dot_size = 8;
    int spacing = 25;
    int start_x = (LCD_WIDTH - (num_dots - 1) * spacing) / 2;

    for (int i = 0; i < num_dots; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, dot_size, dot_size);
        lv_obj_set_pos(dot, start_x + i * spacing - dot_size / 2, 86);
        lv_obj_set_style_bg_color(dot, lv_color_hex(theme_accent_color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, dot_size / 2, 0);  // Make it circular

        // Animate each dot with phase offset for wave effect
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, dot);
        lv_anim_set_values(&anim, -100, 100);
        lv_anim_set_time(&anim, 800);
        lv_anim_set_delay(&anim, i * 100);  // Phase offset
        lv_anim_set_exec_cb(&anim, sine_wave_anim_cb);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_playback_time(&anim, 800);
        lv_anim_start(&anim);
    }

    current_display_status = DISPLAY_STATUS_CONNECTING;
}

void lcd_show_data_error(const char* title, const char* message, const char* hint)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Error header
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(header, LCD_WIDTH, 30);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xFF4444), 0);  // Red for errors
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);

    lv_obj_t *header_txt = lv_label_create(header);
    lv_label_set_text(header_txt, title ? title : "Error");
    lv_obj_set_style_text_font(header_txt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(header_txt, lv_color_hex(THEME_TEXT), 0);
    lv_obj_center(header_txt);

    // Error message
    if (message) {
        lv_obj_t *msg = lv_label_create(scr);
        lv_label_set_text(msg, message);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(msg, LCD_WIDTH - 20);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
    }

    // Hint
    if (hint) {
        lv_obj_t *hint_lbl = lv_label_create(scr);
        lv_label_set_text(hint_lbl, hint);
        lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    current_display_status = DISPLAY_STATUS_ERROR;
}

void lcd_show_no_services(const char* message)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *header_bg = lv_label_create(scr);
    lv_obj_set_size(header_bg, LCD_WIDTH, 26);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    lv_obj_t *svc_type = lv_label_create(scr);
    lv_label_set_text(svc_type, "Metro");
    lv_obj_set_style_text_font(svc_type, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(svc_type, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(svc_type, 8, 5);

    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 5);

    // No services message
    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, message ? message : "No current departures");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, LCD_WIDTH - 20);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

    // Hint
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Check timetable for next service");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    current_display_status = DISPLAY_STATUS_NO_SERVICES;
}

void lcd_update_realtime_departures(const tfnsw_departures_t* departures)
{
    if (!departures) return;

    // Copy data (thread-safe)
    memcpy(&realtime_departures, departures, sizeof(tfnsw_departures_t));
    realtime_mode_enabled = true;

    // Set pending flag - actual LVGL update happens in lcd_update() from main loop
    pending_realtime_update = true;
}

// Apply realtime update - called from lcd_update() in main loop (thread-safe for LVGL)
static void lcd_apply_realtime_update(void)
{
    // Only update display if we're on the departure board scene
    if (current_scene != SCENE_DEPARTURE_BOARD) {
        return;
    }

    tfnsw_departures_t* deps = &realtime_departures;

    // Handle different statuses
    switch (deps->status) {
        case TFNSW_STATUS_ERROR_NO_API_KEY:
            lcd_show_api_key_required();
            return;

        case TFNSW_STATUS_FETCHING:
            if (deps->count == 0) {
                lcd_show_fetching();
            }
            return;

        case TFNSW_STATUS_ERROR_AUTH:
            lcd_show_data_error("Invalid API Key",
                               "Your API key is not valid",
                               "Check key at opendata.transport.nsw.gov.au");
            return;

        case TFNSW_STATUS_ERROR_RATE_LIMIT:
            lcd_show_data_error("Rate Limited",
                               "Too many requests",
                               "Try again in a few minutes");
            return;

        case TFNSW_STATUS_ERROR_NETWORK:
        case TFNSW_STATUS_ERROR_TIMEOUT:
            if (deps->count == 0) {
                lcd_show_data_error("Network Error",
                                   deps->error_message[0] ? deps->error_message : "Connection failed",
                                   "Check WiFi connection");
            }
            return;

        case TFNSW_STATUS_ERROR_SERVER:
            lcd_show_data_error("Server Error",
                               "TfNSW service unavailable",
                               "Try again later");
            return;

        case TFNSW_STATUS_ERROR_NO_DATA:
            lcd_show_no_services(deps->suspension_message[0] ?
                                deps->suspension_message : NULL);
            return;

        case TFNSW_STATUS_SUCCESS:
        case TFNSW_STATUS_IDLE:
            break;

        default:
            if (deps->count == 0) {
                lcd_show_data_error("Error",
                                   deps->error_message[0] ? deps->error_message : "Unknown error",
                                   "Press button to retry");
            }
            return;
    }

    // Show realtime departure board
    if (deps->count > 0) {
        lcd_show_realtime_metro_board();
    } else {
        lcd_show_no_services(NULL);
    }
}

// Enhanced departure board with realtime data
static void lcd_show_realtime_metro_board(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tfnsw_departures_t* deps = &realtime_departures;

    // ===== HEADER BAR =====
    lv_obj_t *header_bg = lv_label_create(scr);
    lv_obj_set_size(header_bg, LCD_WIDTH, 26);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    lv_obj_t *svc_type = lv_label_create(scr);
    lv_label_set_text(svc_type, "Metro");
    lv_obj_set_style_text_font(svc_type, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(svc_type, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(svc_type, 8, 5);

    // Live indicator or status
    if (deps->status == TFNSW_STATUS_SUCCESS) {
        lv_obj_t *live_indicator = lv_label_create(scr);
        lv_label_set_text(live_indicator, "LIVE");
        lv_obj_set_style_text_font(live_indicator, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(live_indicator, lv_color_hex(THEME_BG), 0);
        lv_obj_set_pos(live_indicator, 55, 7);
    }

    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 5);

    current_display_status = DISPLAY_STATUS_LIVE;

    if (deps->count == 0) {
        // No departures
        lv_obj_t *no_svc = lv_label_create(scr);
        lv_label_set_text(no_svc, "See platform screens");
        lv_obj_set_style_text_font(no_svc, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(no_svc, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(no_svc, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // ===== FIRST DEPARTURE (MAIN) =====
    tfnsw_departure_t* first = &deps->departures[0];

    // Destination - validate before display
    const char *dest_str = first->destination;
    if (!dest_str || dest_str[0] == '\0' || strlen(dest_str) < 3) {
        dest_str = "Unknown";  // Fallback for empty/corrupt destinations
    }
    lv_obj_t *dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, dest_str);
    lv_obj_set_style_text_font(dest_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dest_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_pos(dest_lbl, 10, 30);

    // Minutes display
    char mins_str[24];
    if (first->mins_to_departure <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else if (first->mins_to_departure == 1) {
        snprintf(mins_str, sizeof(mins_str), "1 min");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%d min", first->mins_to_departure);
    }

    lv_obj_t *mins_lbl = lv_label_create(scr);
    lv_label_set_text(mins_lbl, mins_str);
    lv_obj_set_style_text_font(mins_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mins_lbl, lv_color_hex(theme_accent_color), 0);
    lv_obj_align(mins_lbl, LV_ALIGN_TOP_RIGHT, -10, 30);

    // Realtime indicator and delay
    int y_indicator = 56;
    if (first->is_realtime) {
        lv_obj_t *rt_dot = lv_label_create(scr);
        lv_obj_set_size(rt_dot, 6, 6);
        lv_obj_set_pos(rt_dot, 10, y_indicator + 4);
        lv_obj_set_style_bg_color(rt_dot, lv_color_hex(0x00FF00), 0);  // Green dot
        lv_obj_set_style_bg_opa(rt_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(rt_dot, 3, 0);

        lv_obj_t *rt_lbl = lv_label_create(scr);
        if (first->delay_seconds > 60) {
            char delay_str[32];
            snprintf(delay_str, sizeof(delay_str), "Delayed +%d min", first->delay_seconds / 60);
            lv_label_set_text(rt_lbl, delay_str);
            lv_obj_set_style_text_color(rt_lbl, lv_color_hex(0xFF8800), 0);  // Orange for delay
        } else if (first->delay_seconds < -60) {
            lv_label_set_text(rt_lbl, "Running early");
            lv_obj_set_style_text_color(rt_lbl, lv_color_hex(0x00AAFF), 0);  // Blue for early
        } else {
            lv_label_set_text(rt_lbl, "On time");
            lv_obj_set_style_text_color(rt_lbl, lv_color_hex(0x00FF00), 0);  // Green for on time
        }
        lv_obj_set_style_text_font(rt_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(rt_lbl, 20, y_indicator);
    } else {
        // Scheduled time indicator
        lv_obj_t *sched_lbl = lv_label_create(scr);
        lv_label_set_text(sched_lbl, "Scheduled");
        lv_obj_set_style_text_font(sched_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sched_lbl, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_set_pos(sched_lbl, 10, y_indicator);
    }

    // Platform if available
    if (first->platform[0] != '\0') {
        lv_obj_t *platform = lv_label_create(scr);
        char plat_str[16];
        snprintf(plat_str, sizeof(plat_str), "Plat %s", first->platform);
        lv_label_set_text(platform, plat_str);
        lv_obj_set_style_text_font(platform, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(platform, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(platform, LV_ALIGN_TOP_RIGHT, -10, y_indicator);
    }

    // ===== CALLING STATIONS (scrolling) =====
    if (first->calling_stations[0] != '\0') {
        lv_obj_t *calling_lbl = lv_label_create(scr);
        lv_label_set_text(calling_lbl, first->calling_stations);
        lv_obj_set_style_text_font(calling_lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(calling_lbl, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(calling_lbl, LCD_WIDTH - 20);
        lv_label_set_long_mode(calling_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_speed(calling_lbl, 20, 0);  // Slow scroll
        lv_obj_set_pos(calling_lbl, 10, 70);
    }

    // ===== SEPARATOR =====
    lv_obj_t *line = lv_label_create(scr);
    lv_obj_set_size(line, LCD_WIDTH, 1);
    lv_obj_set_pos(line, 0, 85);
    lv_obj_set_style_bg_color(line, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    // ===== NEXT SERVICES =====
    int y_offset = 92;
    int row_height = 20;

    for (int i = 1; i < deps->count && i < 4; i++) {
        tfnsw_departure_t* dep = &deps->departures[i];
        int y = y_offset + (i - 1) * row_height;

        // Destination - validate before display
        const char *next_dest_str = dep->destination;
        if (!next_dest_str || next_dest_str[0] == '\0' || strlen(next_dest_str) < 3) {
            next_dest_str = "Unknown";
        }
        lv_obj_t *next_dest = lv_label_create(scr);
        lv_label_set_text(next_dest, next_dest_str);
        lv_obj_set_style_text_font(next_dest, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(next_dest, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(next_dest, LCD_WIDTH - 80);
        lv_label_set_long_mode(next_dest, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(next_dest, 10, y);

        // Minutes
        char next_mins[16];
        tfnsw_format_departure_time(dep->mins_to_departure, next_mins, sizeof(next_mins));

        lv_obj_t *next_time = lv_label_create(scr);
        lv_label_set_text(next_time, next_mins);
        lv_obj_set_style_text_font(next_time, &lv_font_montserrat_14, 0);

        // Color based on realtime/delay status
        uint32_t time_color = theme_accent_color;
        if (dep->is_delayed) {
            time_color = 0xFF8800;  // Orange for delayed
        }
        lv_obj_set_style_text_color(next_time, lv_color_hex(time_color), 0);
        lv_obj_align(next_time, LV_ALIGN_TOP_RIGHT, -10, y);

        // Small realtime dot
        if (dep->is_realtime) {
            lv_obj_t *rt_dot = lv_label_create(scr);
            lv_obj_set_size(rt_dot, 4, 4);
            lv_obj_set_pos(rt_dot, 4, y + 6);
            lv_obj_set_style_bg_color(rt_dot, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(rt_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(rt_dot, 2, 0);
        }
    }

    // ===== STATUS BAR (bottom) =====
    if (deps->consecutive_errors > 0) {
        lv_obj_t *status_bar = lv_label_create(scr);
        char status_str[32];
        snprintf(status_str, sizeof(status_str), "Update pending...");
        lv_label_set_text(status_bar, status_str);
        lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF8800), 0);
        lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -5);
    }
}

// ============================================================================
// Dual-Direction Update Functions
// ============================================================================

void lcd_update_dual_departures(const tfnsw_dual_departures_t* departures)
{
    if (!departures) return;

    // Copy data (thread-safe)
    memcpy(&dual_departures, departures, sizeof(tfnsw_dual_departures_t));
    dual_mode_enabled = true;

    // Set pending flag - actual LVGL update happens in lcd_update() from main loop
    pending_dual_update = true;
}

// Apply dual update - called from lcd_update() in main loop (thread-safe for LVGL)
static void lcd_apply_dual_update(void)
{
    // Only update display if we're on the departure board scene
    if (current_scene != SCENE_DEPARTURE_BOARD) {
        return;
    }

    tfnsw_dual_departures_t* deps = &dual_departures;

    // Handle different statuses
    switch (deps->status) {
        case TFNSW_STATUS_ERROR_NO_API_KEY:
            lcd_show_api_key_required();
            return;

        case TFNSW_STATUS_FETCHING:
            if (deps->northbound_count == 0 && deps->southbound_count == 0) {
                lcd_show_fetching();
            }
            return;

        case TFNSW_STATUS_ERROR_AUTH:
            lcd_show_data_error("Invalid API Key",
                               "Your API key is not valid",
                               "Check key at opendata.transport.nsw.gov.au");
            return;

        case TFNSW_STATUS_ERROR_RATE_LIMIT:
            lcd_show_data_error("Rate Limited",
                               "Too many requests",
                               "Try again in a few minutes");
            return;

        case TFNSW_STATUS_ERROR_NETWORK:
        case TFNSW_STATUS_ERROR_TIMEOUT:
            // If we have cached data, show it with a warning instead of error
            if (deps->northbound_count > 0 || deps->southbound_count > 0) {
                break;  // Fall through to show cached data
            }
            lcd_show_data_error("Network Error",
                               deps->error_message[0] ? deps->error_message : "Connection failed",
                               "Check WiFi connection");
            return;

        case TFNSW_STATUS_ERROR_SERVER:
            // If we have cached data, show it with a warning
            if (deps->northbound_count > 0 || deps->southbound_count > 0) {
                break;  // Fall through to show cached data
            }
            lcd_show_data_error("Server Error",
                               "TfNSW service unavailable",
                               "Try again later");
            return;

        case TFNSW_STATUS_ERROR_RESPONSE_TOO_LARGE:
            // If we have cached data, show it with a warning
            if (deps->northbound_count > 0 || deps->southbound_count > 0) {
                break;  // Fall through to show cached data
            }
            lcd_show_data_error("Data Error",
                               "Response too large",
                               "Retrying automatically");
            return;

        case TFNSW_STATUS_ERROR_TIME_NOT_SYNCED:
            // Show fetching while time syncs - this is usually transient
            if (deps->northbound_count == 0 && deps->southbound_count == 0) {
                lcd_show_fetching();
            }
            return;

        case TFNSW_STATUS_ERROR_PARSE:
            // If we have cached data, show it with a warning
            if (deps->northbound_count > 0 || deps->southbound_count > 0) {
                break;  // Fall through to show cached data
            }
            lcd_show_data_error("Data Error",
                               deps->error_message[0] ? deps->error_message : "Parse failed",
                               "Retrying automatically");
            return;

        case TFNSW_STATUS_ERROR_NO_DATA:
            lcd_show_no_services(deps->suspension_message[0] ?
                                deps->suspension_message : NULL);
            return;

        case TFNSW_STATUS_SUCCESS:
        case TFNSW_STATUS_SUCCESS_CACHED:
        case TFNSW_STATUS_IDLE:
            break;

        default:
            if (deps->northbound_count == 0 && deps->southbound_count == 0) {
                lcd_show_data_error("Error",
                                   deps->error_message[0] ? deps->error_message : "Unknown error",
                                   "Press button to retry");
            }
            return;
    }

    // Show unified departure board with both directions
    int total_count = deps->northbound_count + deps->southbound_count;
    if (total_count > 0) {
        lcd_show_dual_metro_board();
    } else {
        lcd_show_no_services(NULL);
    }
}

// Helper: Merge and sort dual departures into a single array
static int merge_dual_departures(tfnsw_dual_departures_t* deps, tfnsw_departure_t* merged, int max_count)
{
    int total = 0;
    int ni = 0, si = 0;

    // Merge sort both arrays by mins_to_departure
    while (total < max_count && (ni < deps->northbound_count || si < deps->southbound_count)) {
        bool use_north = false;

        if (ni >= deps->northbound_count) {
            use_north = false;
        } else if (si >= deps->southbound_count) {
            use_north = true;
        } else {
            // Compare departure times
            use_north = (deps->northbound[ni].mins_to_departure <=
                        deps->southbound[si].mins_to_departure);
        }

        if (use_north) {
            memcpy(&merged[total], &deps->northbound[ni], sizeof(tfnsw_departure_t));
            ni++;
        } else {
            memcpy(&merged[total], &deps->southbound[si], sizeof(tfnsw_departure_t));
            si++;
        }
        total++;
    }

    return total;
}

// Unified departure board showing both directions in single list
static void lcd_show_dual_metro_board(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tfnsw_dual_departures_t* deps = &dual_departures;

    // Merge departures into sorted list (initialize to zeros first to prevent garbage display)
    tfnsw_departure_t merged[TFNSW_MAX_DEPARTURES];
    memset(merged, 0, sizeof(merged));
    int total_count = merge_dual_departures(deps, merged, TFNSW_MAX_DEPARTURES);

    // ===== HEADER BAR =====
    lv_obj_t *header_bg = lv_label_create(scr);
    lv_obj_set_size(header_bg, LCD_WIDTH, 24);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    lv_obj_t *svc_type = lv_label_create(scr);
    lv_label_set_text(svc_type, "Victoria Cross");
    lv_obj_set_style_text_font(svc_type, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(svc_type, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(svc_type, 8, 5);

    // Status indicator - show data quality
    lv_obj_t *status_dot = lv_label_create(scr);
    lv_obj_set_size(status_dot, 6, 6);
    lv_obj_set_pos(status_dot, LCD_WIDTH - 50, 9);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_dot, 3, 0);

    if (deps->status == TFNSW_STATUS_SUCCESS) {
        // Green dot for live data
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x00FF00), 0);
    } else if (deps->status == TFNSW_STATUS_SUCCESS_CACHED || deps->is_cached_fallback) {
        // Yellow dot for cached data
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xFFAA00), 0);
    } else if (deps->is_stale) {
        // Orange dot for stale data
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xFF6600), 0);
    } else {
        // Red dot for error
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xFF0000), 0);
    }

    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 5);

    current_display_status = DISPLAY_STATUS_LIVE;

    if (total_count == 0) {
        lv_obj_t *no_svc = lv_label_create(scr);
        lv_label_set_text(no_svc, "See platform screens");
        lv_obj_set_style_text_font(no_svc, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(no_svc, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(no_svc, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // ===== FIRST DEPARTURE (FEATURED) =====
    tfnsw_departure_t* first = &merged[0];

    // Direction indicator (small arrow)
    lv_obj_t *dir_indicator = lv_label_create(scr);
    if (first->direction == TFNSW_DIRECTION_NORTHBOUND) {
        lv_label_set_text(dir_indicator, "^");  // Up arrow for north
    } else {
        lv_label_set_text(dir_indicator, "v");  // Down arrow for south
    }
    lv_obj_set_style_text_font(dir_indicator, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(dir_indicator, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_pos(dir_indicator, 8, 28);

    // Destination (larger) - validate before display
    const char *dest_str = first->destination;
    if (!dest_str || dest_str[0] == '\0' || strlen(dest_str) < 3) {
        dest_str = "Unknown";  // Fallback for empty/corrupt destinations
    }
    lv_obj_t *dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, dest_str);
    lv_obj_set_style_text_font(dest_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(dest_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(dest_lbl, LCD_WIDTH - 100);
    lv_label_set_long_mode(dest_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(dest_lbl, 22, 27);

    // Minutes display (featured size)
    char mins_str[16];
    if (first->mins_to_departure <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else if (first->mins_to_departure == 1) {
        snprintf(mins_str, sizeof(mins_str), "1min");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%dmin", first->mins_to_departure);
    }

    lv_obj_t *mins_lbl = lv_label_create(scr);
    lv_label_set_text(mins_lbl, mins_str);
    lv_obj_set_style_text_font(mins_lbl, &lv_font_montserrat_20, 0);

    // Color based on status
    uint32_t mins_color = theme_accent_color;
    if (first->mins_to_departure <= 0) {
        mins_color = 0x00FF00;  // Green for NOW
    } else if (first->is_delayed) {
        mins_color = 0xFF8800;  // Orange for delayed
    }
    lv_obj_set_style_text_color(mins_lbl, lv_color_hex(mins_color), 0);
    lv_obj_align(mins_lbl, LV_ALIGN_TOP_RIGHT, -8, 27);

    // Realtime/scheduled indicator + delay info
    lv_obj_t *status_lbl = lv_label_create(scr);
    if (first->is_realtime) {
        if (first->delay_seconds > 60) {
            char delay_str[24];
            snprintf(delay_str, sizeof(delay_str), "+%dm late", first->delay_seconds / 60);
            lv_label_set_text(status_lbl, delay_str);
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xFF8800), 0);
        } else if (first->delay_seconds < -60) {
            lv_label_set_text(status_lbl, "Early");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00AAFF), 0);
        } else {
            lv_label_set_text(status_lbl, "On time");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00FF00), 0);
        }
    } else {
        lv_label_set_text(status_lbl, "Scheduled");
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(THEME_SECONDARY), 0);
    }
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(status_lbl, 22, 48);

    // ===== SEPARATOR =====
    lv_obj_t *line = lv_label_create(scr);
    lv_obj_set_size(line, LCD_WIDTH - 16, 1);
    lv_obj_set_pos(line, 8, 64);
    lv_obj_set_style_bg_color(line, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    // ===== FOLLOWING SERVICES (compact list) =====
    int y_start = 70;
    int row_height = 24;
    int max_rows = 4;  // Show up to 4 more services

    for (int i = 1; i < total_count && i <= max_rows; i++) {
        tfnsw_departure_t* dep = &merged[i];
        int y = y_start + (i - 1) * row_height;

        // Direction indicator
        lv_obj_t *row_dir = lv_label_create(scr);
        if (dep->direction == TFNSW_DIRECTION_NORTHBOUND) {
            lv_label_set_text(row_dir, "^");
        } else {
            lv_label_set_text(row_dir, "v");
        }
        lv_obj_set_style_text_font(row_dir, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(row_dir, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_set_pos(row_dir, 8, y + 2);

        // Destination - validate before display
        const char *row_dest_str = dep->destination;
        if (!row_dest_str || row_dest_str[0] == '\0' || strlen(row_dest_str) < 3) {
            row_dest_str = "Unknown";
        }
        lv_obj_t *row_dest = lv_label_create(scr);
        lv_label_set_text(row_dest, row_dest_str);
        lv_obj_set_style_text_font(row_dest, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(row_dest, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(row_dest, LCD_WIDTH - 80);
        lv_label_set_long_mode(row_dest, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(row_dest, 22, y);

        // Minutes
        char row_mins[16];
        if (dep->mins_to_departure <= 0) {
            snprintf(row_mins, sizeof(row_mins), "NOW");
        } else {
            snprintf(row_mins, sizeof(row_mins), "%d min", dep->mins_to_departure);
        }

        lv_obj_t *row_time = lv_label_create(scr);
        lv_label_set_text(row_time, row_mins);
        lv_obj_set_style_text_font(row_time, &lv_font_montserrat_14, 0);

        // Color based on status
        uint32_t row_color = theme_accent_color;
        if (dep->mins_to_departure <= 0) {
            row_color = 0x00FF00;
        } else if (dep->is_delayed) {
            row_color = 0xFF8800;
        }
        lv_obj_set_style_text_color(row_time, lv_color_hex(row_color), 0);
        lv_obj_align(row_time, LV_ALIGN_TOP_RIGHT, -8, y);

        // Small realtime indicator
        if (dep->is_realtime) {
            lv_obj_t *rt_dot = lv_label_create(scr);
            lv_obj_set_size(rt_dot, 4, 4);
            lv_obj_set_pos(rt_dot, 18, y + 6);
            lv_obj_set_style_bg_color(rt_dot, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(rt_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(rt_dot, 2, 0);
        }
    }

    // ===== STATUS INDICATOR (bottom) =====
    lv_obj_t *status_bar = lv_label_create(scr);
    lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_12, 0);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    if (deps->is_cached_fallback) {
        // Show cached data warning with age
        char status_msg[64];
        if (deps->data_age_seconds > 60) {
            snprintf(status_msg, sizeof(status_msg), "Cached data (%dm old)", deps->data_age_seconds / 60);
        } else {
            snprintf(status_msg, sizeof(status_msg), "Cached data");
        }
        lv_label_set_text(status_bar, status_msg);
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFFAA00), 0);
    } else if (deps->is_stale) {
        // Show stale data warning
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "Last update: %dm ago", deps->data_age_seconds / 60);
        lv_label_set_text(status_bar, status_msg);
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF6600), 0);
    } else if (deps->consecutive_errors > 0) {
        // Show error with retry count
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "Retrying... (%d)", deps->consecutive_errors);
        lv_label_set_text(status_bar, status_msg);
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF8800), 0);
    } else if (deps->status == TFNSW_STATUS_SUCCESS) {
        // Hide status bar when everything is normal (or show subtle live indicator)
        lv_label_set_text(status_bar, "");
    } else {
        lv_label_set_text(status_bar, "");
    }
}

// ============================================================================
// Simple Metro Board (Single Direction)
// ============================================================================

static void lcd_show_simple_metro_board(bool northbound)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tfnsw_departures_t *deps = northbound ? &northbound_data : &southbound_data;
    const char *station_name = northbound ? "Victoria Cross" : "Crows Nest";
    const char *direction_text = northbound ? "Tallawong" : "Sydenham";

    // ===== HEADER BAR =====
    lv_obj_t *header_bg = lv_label_create(scr);
    lv_obj_set_size(header_bg, LCD_WIDTH, 24);
    lv_obj_set_pos(header_bg, 0, 0);
    lv_obj_set_style_bg_color(header_bg, lv_color_hex(theme_accent_color), 0);
    lv_obj_set_style_bg_opa(header_bg, LV_OPA_COVER, 0);

    // Station name
    lv_obj_t *svc_type = lv_label_create(scr);
    lv_label_set_text(svc_type, station_name);
    lv_obj_set_style_text_font(svc_type, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(svc_type, lv_color_hex(THEME_BG), 0);
    lv_obj_set_pos(svc_type, 8, 5);

    // Status indicators (right side of header)
    int indicator_x = LCD_WIDTH - 58;

    // Refresh indicator - shows when fetching
    if (tfnsw_is_fetching()) {
        lv_obj_t *refresh_icon = lv_label_create(scr);
        lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(refresh_icon, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(refresh_icon, lv_color_hex(THEME_BG), 0);
        lv_obj_set_pos(refresh_icon, indicator_x, 5);
        indicator_x += 14;
    }

    // Live indicator dot
    lv_obj_t *live_dot = lv_label_create(scr);
    lv_obj_set_size(live_dot, 6, 6);
    lv_obj_set_pos(live_dot, indicator_x, 9);
    lv_obj_set_style_bg_opa(live_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(live_dot, 3, 0);

    // Color based on data state
    bool has_realtime = false;
    for (int i = 0; i < deps->count && i < 3; i++) {
        if (deps->departures[i].is_realtime) {
            has_realtime = true;
            break;
        }
    }

    if (deps->status == TFNSW_STATUS_SUCCESS && has_realtime) {
        lv_obj_set_style_bg_color(live_dot, lv_color_hex(0x00FF00), 0);  // Green = live
    } else if (deps->status == TFNSW_STATUS_SUCCESS) {
        lv_obj_set_style_bg_color(live_dot, lv_color_hex(0xFFFF00), 0);  // Yellow = scheduled only
    } else if (deps->status == TFNSW_STATUS_ERROR_PARSE ||
               deps->status == TFNSW_STATUS_ERROR_NETWORK) {
        lv_obj_set_style_bg_color(live_dot, lv_color_hex(0xFF0000), 0);  // Red = error
    } else {
        lv_obj_set_style_bg_color(live_dot, lv_color_hex(0xFF8800), 0);  // Orange = other
    }

    // Current time
    lv_obj_t *time_now = lv_label_create(scr);
    lv_label_set_text(time_now, get_current_time_str());
    lv_obj_set_style_text_font(time_now, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_now, lv_color_hex(THEME_BG), 0);
    lv_obj_align(time_now, LV_ALIGN_TOP_RIGHT, -8, 5);

    current_display_status = DISPLAY_STATUS_LIVE;

    // Error/no data handling
    if (deps->count == 0) {
        lv_obj_t *no_svc = lv_label_create(scr);
        if (deps->error_message[0] != '\0') {
            lv_label_set_text(no_svc, deps->error_message);
        } else {
            lv_label_set_text(no_svc, "No services");
        }
        lv_obj_set_style_text_font(no_svc, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(no_svc, lv_color_hex(THEME_SECONDARY), 0);
        lv_obj_align(no_svc, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // ===== DIRECTION HEADER =====
    lv_obj_t *dir_label = lv_label_create(scr);
    char dir_str[32];
    snprintf(dir_str, sizeof(dir_str), "%s %s", northbound ? "^" : "v", direction_text);
    lv_label_set_text(dir_label, dir_str);
    lv_obj_set_style_text_font(dir_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dir_label, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_pos(dir_label, 8, 28);

    // ===== FIRST DEPARTURE (FEATURED) =====
    tfnsw_departure_t *first = &deps->departures[0];

    // Destination
    lv_obj_t *dest_lbl = lv_label_create(scr);
    lv_label_set_text(dest_lbl, first->destination[0] ? first->destination : "Unknown");
    lv_obj_set_style_text_font(dest_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(dest_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_width(dest_lbl, LCD_WIDTH - 90);
    lv_label_set_long_mode(dest_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(dest_lbl, 8, 42);

    // Minutes display
    char mins_str[16];
    if (first->mins_to_departure <= 0) {
        snprintf(mins_str, sizeof(mins_str), "NOW");
    } else if (first->mins_to_departure == 1) {
        snprintf(mins_str, sizeof(mins_str), "1min");
    } else {
        snprintf(mins_str, sizeof(mins_str), "%dmin", first->mins_to_departure);
    }

    lv_obj_t *mins_lbl = lv_label_create(scr);
    lv_label_set_text(mins_lbl, mins_str);
    lv_obj_set_style_text_font(mins_lbl, &lv_font_montserrat_20, 0);

    uint32_t mins_color = theme_accent_color;
    if (first->mins_to_departure <= 0) {
        mins_color = 0x00FF00;
    } else if (first->is_delayed) {
        mins_color = 0xFF8800;
    }
    lv_obj_set_style_text_color(mins_lbl, lv_color_hex(mins_color), 0);
    lv_obj_align(mins_lbl, LV_ALIGN_TOP_RIGHT, -8, 42);

    // Realtime indicator + status
    lv_obj_t *status_lbl = lv_label_create(scr);
    if (first->is_realtime) {
        if (first->delay_seconds > 60) {
            char delay_str[24];
            snprintf(delay_str, sizeof(delay_str), "+%dm late", first->delay_seconds / 60);
            lv_label_set_text(status_lbl, delay_str);
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xFF8800), 0);
        } else if (first->delay_seconds < -60) {
            lv_label_set_text(status_lbl, "Early");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00AAFF), 0);
        } else {
            lv_label_set_text(status_lbl, "LIVE - On time");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x00FF00), 0);
        }
    } else {
        lv_label_set_text(status_lbl, "Scheduled");
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(THEME_SECONDARY), 0);
    }
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(status_lbl, 8, 64);

    // ===== SEPARATOR =====
    lv_obj_t *line = lv_label_create(scr);
    lv_obj_set_size(line, LCD_WIDTH - 16, 1);
    lv_obj_set_pos(line, 8, 80);
    lv_obj_set_style_bg_color(line, lv_color_hex(THEME_SECONDARY), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    // ===== FOLLOWING SERVICES =====
    int y_start = 86;
    int row_height = 22;
    int max_rows = 3;

    for (int i = 1; i < deps->count && i <= max_rows; i++) {
        tfnsw_departure_t *dep = &deps->departures[i];
        int y = y_start + (i - 1) * row_height;

        // Live indicator dot
        if (dep->is_realtime) {
            lv_obj_t *rt_dot = lv_label_create(scr);
            lv_obj_set_size(rt_dot, 4, 4);
            lv_obj_set_pos(rt_dot, 8, y + 5);
            lv_obj_set_style_bg_color(rt_dot, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_bg_opa(rt_dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(rt_dot, 2, 0);
        }

        // Destination
        lv_obj_t *row_dest = lv_label_create(scr);
        lv_label_set_text(row_dest, dep->destination[0] ? dep->destination : "Unknown");
        lv_obj_set_style_text_font(row_dest, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(row_dest, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_width(row_dest, LCD_WIDTH - 80);
        lv_label_set_long_mode(row_dest, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(row_dest, 16, y);

        // Minutes
        char row_mins[16];
        if (dep->mins_to_departure <= 0) {
            snprintf(row_mins, sizeof(row_mins), "NOW");
        } else {
            snprintf(row_mins, sizeof(row_mins), "%d min", dep->mins_to_departure);
        }

        lv_obj_t *row_time = lv_label_create(scr);
        lv_label_set_text(row_time, row_mins);
        lv_obj_set_style_text_font(row_time, &lv_font_montserrat_14, 0);

        uint32_t row_color = theme_accent_color;
        if (dep->mins_to_departure <= 0) {
            row_color = 0x00FF00;
        } else if (dep->is_delayed) {
            row_color = 0xFF8800;
        }
        lv_obj_set_style_text_color(row_time, lv_color_hex(row_color), 0);
        lv_obj_align(row_time, LV_ALIGN_TOP_RIGHT, -8, y);
    }

    // ===== BOTTOM STATUS =====
    lv_obj_t *status_bar = lv_label_create(scr);
    lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_12, 0);
    lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    if (deps->status != TFNSW_STATUS_SUCCESS && deps->error_message[0] != '\0') {
        lv_label_set_text(status_bar, deps->error_message);
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF8800), 0);
    } else {
        lv_label_set_text(status_bar, "");
    }
}

static void lcd_apply_simple_update(bool northbound)
{
    // Refresh the appropriate metro scene
    if (northbound && current_scene == SCENE_METRO_NORTH) {
        lcd_refresh_scene();
    } else if (!northbound && current_scene == SCENE_METRO_SOUTH) {
        lcd_refresh_scene();
    }
}

// Public function to enable simple mode and set data
void lcd_set_simple_mode(bool enabled)
{
    simple_mode_enabled = enabled;
    if (enabled) {
        dual_mode_enabled = false;
        realtime_mode_enabled = false;
    }
}

void lcd_update_northbound_departures(const tfnsw_departures_t *departures)
{
    if (!departures) return;
    memcpy(&northbound_data, departures, sizeof(tfnsw_departures_t));
    pending_north_update = true;
}

void lcd_update_southbound_departures(const tfnsw_departures_t *departures)
{
    if (!departures) return;
    memcpy(&southbound_data, departures, sizeof(tfnsw_departures_t));
    pending_south_update = true;
}
