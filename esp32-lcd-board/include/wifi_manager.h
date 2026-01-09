#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

// WiFi event callback type
typedef void (*wifi_event_cb_t)(void);

// Initialize WiFi subsystem
esp_err_t wifi_init(void);

// Try to connect with stored credentials, start AP if none stored
esp_err_t wifi_connect(void);

// Start AP mode for configuration
esp_err_t wifi_start_ap(void);

// Stop AP mode
esp_err_t wifi_stop_ap(void);

// Save WiFi credentials to NVS
esp_err_t wifi_save_credentials(const char* ssid, const char* password);

// Clear stored credentials
esp_err_t wifi_clear_credentials(void);

// Load stored credentials
esp_err_t wifi_load_credentials(char* ssid, size_t ssid_len, char* password, size_t pass_len);

// Check if connected to WiFi
bool wifi_is_connected(void);

// Get current IP address (returns static buffer)
const char* wifi_get_ip(void);

// Get current SSID (returns static buffer)
const char* wifi_get_ssid(void);

// Get signal strength (RSSI)
int8_t wifi_get_rssi(void);

// Set callback for connection events
void wifi_set_connected_callback(wifi_event_cb_t cb);

// Set callback for AP mode events
void wifi_set_ap_callback(wifi_event_cb_t cb);

#endif // WIFI_MANAGER_H
