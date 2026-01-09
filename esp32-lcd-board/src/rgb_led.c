#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "esp_log.h"

#include "config.h"
#include "rgb_led.h"

static const char *TAG = "rgb_led";

static led_strip_handle_t led_strip = NULL;

// Manual color mode - when true, rgb_led_update() does nothing
static bool manual_color_mode = false;

esp_err_t rgb_led_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LED on GPIO %d", RGB_LED_PIN);

    // LED strip configuration for WS2812
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,  // WS2812 uses GRB
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    // RMT configuration for ESP32-C6 (no DMA support)
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = false,  // DMA not supported on ESP32-C6
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start with LED off
    rgb_led_off();

    ESP_LOGI(TAG, "RGB LED initialized successfully");
    return ESP_OK;
}

void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!led_strip) return;

    esp_err_t ret = led_strip_set_pixel(led_strip, 0, red, green, blue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pixel: %s", esp_err_to_name(ret));
        return;
    }

    ret = led_strip_refresh(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh strip: %s", esp_err_to_name(ret));
    }
}

void rgb_led_set_hex(uint32_t hex_color)
{
    // Scale down to ~10% brightness to avoid blinding LED
    uint8_t red = ((hex_color >> 16) & 0xFF) / 10;
    uint8_t green = ((hex_color >> 8) & 0xFF) / 10;
    uint8_t blue = (hex_color & 0xFF) / 10;

    manual_color_mode = true;  // Enable manual mode
    ESP_LOGI(TAG, "LED set to 0x%06X (scaled R:%d G:%d B:%d) manual_mode=true",
             (unsigned int)hex_color, red, green, blue);
    rgb_led_set_color(red, green, blue);
}

void rgb_led_off(void)
{
    if (!led_strip) return;
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

// ============================================================================
// Status Indication Patterns
// ============================================================================

static led_status_t current_status = LED_STATUS_OFF;
static uint32_t animation_tick = 0;
static uint32_t flash_end_tick = 0;
static uint32_t flash_color = 0;
static bool flash_active = false;

void rgb_led_set_status(led_status_t status)
{
    ESP_LOGI(TAG, "LED status set to %d, manual_mode=false", status);
    current_status = status;
    animation_tick = 0;
    manual_color_mode = false;  // Exit manual mode
}

led_status_t rgb_led_get_status(void)
{
    return current_status;
}

void rgb_led_flash(uint32_t color, int duration_ms)
{
    flash_color = color;
    flash_end_tick = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    flash_active = true;

    uint8_t red = (color >> 16) & 0xFF;
    uint8_t green = (color >> 8) & 0xFF;
    uint8_t blue = color & 0xFF;
    rgb_led_set_color(red, green, blue);
}

void rgb_led_update(void)
{
    animation_tick++;

    // Skip status updates when in manual color mode
    if (manual_color_mode) {
        return;
    }

    // Handle flash overlay
    if (flash_active) {
        if (xTaskGetTickCount() >= flash_end_tick) {
            flash_active = false;
        } else {
            return;  // Flash overrides normal status
        }
    }

    switch (current_status) {
        case LED_STATUS_OFF:
            rgb_led_off();
            break;

        case LED_STATUS_CONNECTING:
            rgb_led_set_color(0, 0, 25);  // Dim blue
            break;

        case LED_STATUS_FETCHING:
            rgb_led_set_color(0, 15, 20);  // Dim cyan
            break;

        case LED_STATUS_LIVE:
            rgb_led_set_color(0, 15, 0);  // Dim green
            break;

        case LED_STATUS_LIVE_DELAYED:
            rgb_led_set_color(20, 10, 0);  // Dim orange
            break;

        case LED_STATUS_ERROR_NETWORK:
            rgb_led_set_color(25, 0, 0);  // Dim red
            break;

        case LED_STATUS_ERROR_AUTH:
            rgb_led_set_color(40, 0, 0);  // Red
            break;

        case LED_STATUS_ERROR_RATE:
            rgb_led_set_color(25, 12, 0);  // Orange
            break;

        case LED_STATUS_NO_API_KEY:
            rgb_led_set_color(20, 20, 0);  // Yellow
            break;

        case LED_STATUS_SUCCESS_FLASH:
            rgb_led_set_color(0, 25, 0);  // Green
            if (animation_tick > 50) {
                current_status = LED_STATUS_LIVE;
            }
            break;

        case LED_STATUS_HIGH_SPEED:
            rgb_led_set_color(25, 0, 25);  // Purple
            break;

        default:
            rgb_led_off();
            break;
    }
}
