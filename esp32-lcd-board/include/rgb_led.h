#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>
#include "esp_err.h"

// Initialize the RGB LED
esp_err_t rgb_led_init(void);

// Set RGB color (0-255 for each channel)
void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue);

// Set color using hex value (0xRRGGBB)
void rgb_led_set_hex(uint32_t hex_color);

// Turn off the LED
void rgb_led_off(void);

// Predefined colors
#define RGB_RED     0xFF0000
#define RGB_GREEN   0x00FF00
#define RGB_BLUE    0x0000FF
#define RGB_WHITE   0xFFFFFF
#define RGB_YELLOW  0xFFFF00
#define RGB_CYAN    0x00FFFF
#define RGB_MAGENTA 0xFF00FF
#define RGB_ORANGE  0xFF8000

// ============================================================================
// Status Indication Patterns
// ============================================================================

// LED status modes for departure board
typedef enum {
    LED_STATUS_OFF = 0,
    LED_STATUS_CONNECTING,      // Blue pulsing - WiFi connecting
    LED_STATUS_FETCHING,        // Cyan flash - API data fetching
    LED_STATUS_LIVE,            // Dim green - Live data active
    LED_STATUS_LIVE_DELAYED,    // Dim orange - Live but delayed
    LED_STATUS_ERROR_NETWORK,   // Red slow pulse - Network error
    LED_STATUS_ERROR_AUTH,      // Red fast pulse - Auth error
    LED_STATUS_ERROR_RATE,      // Orange pulse - Rate limited
    LED_STATUS_NO_API_KEY,      // Yellow pulse - Config needed
    LED_STATUS_SUCCESS_FLASH,   // Green flash then dim - Success
    LED_STATUS_HIGH_SPEED,      // Red/blue alternating flash
} led_status_t;

// Set LED to indicate specific status (non-blocking)
void rgb_led_set_status(led_status_t status);

// Get current status mode
led_status_t rgb_led_get_status(void);

// Update LED animation (call from main loop at ~10ms interval)
void rgb_led_update(void);

// Flash the LED briefly (non-blocking, call rgb_led_update to animate)
void rgb_led_flash(uint32_t color, int duration_ms);

#endif // RGB_LED_H
