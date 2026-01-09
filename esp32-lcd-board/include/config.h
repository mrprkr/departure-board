#ifndef CONFIG_H
#define CONFIG_H

// clang-format off

// ============================================================================
// ESP32-C6-LCD-1.47 Configuration
// ============================================================================

#define BOARD_NAME "ESP32-C6-LCD-1.47"
#define FIRMWARE_VERSION "1.0.0"

// ============================================================================
// Display Configuration (ST7789 1.47" 172x320)
// ============================================================================
#define LCD_NATIVE_WIDTH 172
#define LCD_NATIVE_HEIGHT 320
#define LCD_HOST SPI2_HOST

// Rotation: 0=Portrait, 1=Landscape (90°), 2=Portrait inverted, 3=Landscape
// inverted
#define LCD_ROTATION 1

// Effective dimensions after rotation
#if (LCD_ROTATION == 0 || LCD_ROTATION == 2)
#define LCD_WIDTH LCD_NATIVE_WIDTH
#define LCD_HEIGHT LCD_NATIVE_HEIGHT
#else
#define LCD_WIDTH LCD_NATIVE_HEIGHT
#define LCD_HEIGHT LCD_NATIVE_WIDTH
#endif

// LCD Pins (adjust to your board)
#define LCD_PIN_MOSI 6
#define LCD_PIN_SCLK 7
#define LCD_PIN_CS 14
#define LCD_PIN_DC 15
#define LCD_PIN_RST 21
#define LCD_PIN_BL 22

// LCD Settings
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

// ============================================================================
// SD Card Configuration (shares SPI bus with LCD)
// ============================================================================
#define SD_PIN_MOSI 6 // Shared with LCD
#define SD_PIN_MISO 5 // SD card MISO
#define SD_PIN_CLK 7  // Shared with LCD
#define SD_PIN_CS 4   // SD card chip select
#define SD_MOUNT_POINT "/sdcard"

// SD Card file paths (relative to mount point)
#define SETTINGS_FILE "/config.json"
#define LOG_FILE "/system.log"
#define DEPARTURES_FILE "/departures.json"

// ============================================================================
// RGB LED (accent LED on board)
// ============================================================================
#define RGB_LED_PIN 8

// ============================================================================
// Button Configuration (BOOT button on ESP32-C6)
// ============================================================================
#define BUTTON_PIN 9
#define BUTTON_DEBOUNCE_MS 50

// ============================================================================
// WiFi Configuration
// ============================================================================
#define WIFI_AP_SSID "ESP32-LCD-Setup"
#define WIFI_AP_PASS "changeme123"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4
#define WIFI_HOSTNAME "esp32-lcd"

// NVS Keys
#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "password"

// ============================================================================
// Web Server
// ============================================================================
#define WEB_SERVER_PORT 80

// ============================================================================
// TfNSW API Configuration
// ============================================================================
// Get your free API key from https://opendata.transport.nsw.gov.au
// Leave empty ("") to require manual entry via web interface
#define TFNSW_DEFAULT_API_KEY ""

// ============================================================================
// Colors (RGB565)
// ============================================================================
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_RED 0xF800
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN 0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_ORANGE 0xFD20
#define COLOR_GRAY 0x8410
#define COLOR_DARK_GRAY 0x4208 // Dark gray for subtle elements

// Theme colors (RGB888 for LVGL lv_color_hex)
#define THEME_BG 0x000000
#define THEME_TEXT 0xFFFFFF
#define THEME_ACCENT 0xFFE000
#define THEME_SECONDARY 0x666666

#endif // CONFIG_H
