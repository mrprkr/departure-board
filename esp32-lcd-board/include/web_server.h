#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// Command callback type
typedef void (*display_cmd_cb_t)(const char* command, const char* params);
typedef void (*system_cmd_cb_t)(const char* command);
typedef void (*api_key_set_cb_t)(void);

// Start the web server
esp_err_t webserver_start(void);

// Stop the web server
esp_err_t webserver_stop(void);

// Check if server is running
bool webserver_is_running(void);

// Set display command callback
void webserver_set_display_callback(display_cmd_cb_t cb);

// Set system command callback
void webserver_set_system_callback(system_cmd_cb_t cb);

// Set API key set callback (called when API key is saved from web interface)
void webserver_set_api_key_callback(api_key_set_cb_t cb);

#endif // WEB_SERVER_H
