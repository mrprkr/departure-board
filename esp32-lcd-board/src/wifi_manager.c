#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "config.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool is_connected = false;
static char current_ip[16] = "0.0.0.0";
static char current_ssid[33] = "";
static int8_t current_rssi = 0;
static int retry_count = 0;

static wifi_event_cb_t connected_callback = NULL;
static wifi_event_cb_t ap_callback = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                is_connected = false;
                if (retry_count < 5) {
                    ESP_LOGI(TAG, "Retry connecting to AP...");
                    esp_wifi_connect();
                    retry_count++;
                } else {
                    ESP_LOGI(TAG, "Connection failed after retries");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                ESP_LOGI(TAG, "Station joined AP");
                if (ap_callback) ap_callback();
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                ESP_LOGI(TAG, "Station left AP");
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", current_ip);
        retry_count = 0;
        is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Get RSSI
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            current_rssi = ap_info.rssi;
            strncpy(current_ssid, (char*)ap_info.ssid, sizeof(current_ssid) - 1);
        }

        if (connected_callback) connected_callback();
    }
}

esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create event group
    s_wifi_event_group = xEventGroupCreate();

    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t wifi_connect(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    // Try to load saved credentials
    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGI(TAG, "No saved credentials, starting AP mode");
        return wifi_start_ap();
    }

    ESP_LOGI(TAG, "Connecting to saved network: %s", ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s", ssid);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to %s", ssid);
    } else {
        ESP_LOGI(TAG, "Connection timeout");
    }

    // Connection failed, start AP mode
    esp_wifi_stop();
    return wifi_start_ap();
}

esp_err_t wifi_start_ap(void)
{
    ESP_LOGI(TAG, "Starting AP mode: %s", WIFI_AP_SSID);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Get AP IP
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);
    snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&ip_info.ip));

    ESP_LOGI(TAG, "AP started. IP: %s", current_ip);
    return ESP_OK;
}

esp_err_t wifi_stop_ap(void)
{
    return esp_wifi_stop();
}

esp_err_t wifi_save_credentials(const char* ssid, const char* password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_PASS, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials saved");
    return err;
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;

    nvs_erase_key(nvs_handle, NVS_KEY_SSID);
    nvs_erase_key(nvs_handle, NVS_KEY_PASS);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "WiFi credentials cleared");
    return ESP_OK;
}

esp_err_t wifi_load_credentials(char* ssid, size_t ssid_len, char* password, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_PASS, password, &pass_len);
    nvs_close(nvs_handle);

    return err;
}

bool wifi_is_connected(void)
{
    return is_connected;
}

const char* wifi_get_ip(void)
{
    return current_ip;
}

const char* wifi_get_ssid(void)
{
    return current_ssid;
}

int8_t wifi_get_rssi(void)
{
    return current_rssi;
}

void wifi_set_connected_callback(wifi_event_cb_t cb)
{
    connected_callback = cb;
}

void wifi_set_ap_callback(wifi_event_cb_t cb)
{
    ap_callback = cb;
}
