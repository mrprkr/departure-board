#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "tfnsw_client.h"

static const char *TAG = "tfnsw";

// ============================================================================
// NVS Storage Keys
// ============================================================================
#define TFNSW_NVS_NAMESPACE "tfnsw"
#define TFNSW_NVS_KEY_API "api_key"

// ============================================================================
// Internal State
// ============================================================================

static char api_key[512] = {0}; // JWT tokens can be 200-300+ bytes
static bool initialized = false;
static SemaphoreHandle_t data_mutex = NULL;
static tfnsw_departures_t current_departures = {0};
static tfnsw_dual_departures_t current_dual_departures = {0};

// Background fetch task
static TaskHandle_t fetch_task_handle = NULL;
static volatile bool fetch_task_running = false;
static volatile bool force_refresh_flag = false;
static void (*update_callback)(const tfnsw_departures_t *departures) = NULL;
static void (*dual_update_callback)(const tfnsw_dual_departures_t *departures) =
    NULL;
static bool dual_mode_enabled = false;

// Simple mode - separate northbound/southbound/artarmon data
static tfnsw_departures_t northbound_departures = {0};
static tfnsw_departures_t southbound_departures = {0};
static tfnsw_departures_t artarmon_departures = {0};
static void (*north_update_callback)(const tfnsw_departures_t *departures) = NULL;
static void (*south_update_callback)(const tfnsw_departures_t *departures) = NULL;
static void (*artarmon_update_callback)(const tfnsw_departures_t *departures) = NULL;
static bool simple_mode_enabled = false;
static volatile bool is_currently_fetching = false;

// Single-view mode - only fetch for active view (forward declarations)
static char active_stop_id[16] = {0};
static volatile bool single_view_mode_enabled = false;
static void (*single_view_callback)(const tfnsw_departures_t *departures) = NULL;
static tfnsw_departures_t single_view_departures = {0};

// HTTP response buffer - Metro responses are ~1KB, but Sydney Trains vary wildly (15-35KB per departure!)
// 32KB balances train support with heap requirements (mbedTLS needs ~16KB for TLS read buffer)
#define HTTP_BUFFER_SIZE 32768  // 32KB - balance between train support and heap
#define HTTP_BUFFER_WARNING_THRESHOLD 28000  // Warn if response exceeds this
#define STALE_DATA_THRESHOLD_MS 120000  // 2 minutes = stale data
#define MAX_HTTP_RETRIES 3
#define HTTP_RETRY_DELAY_MS 1000
static char *http_buffer = NULL;
static int http_buffer_len = 0;
static bool http_buffer_overflow = false;
static int64_t last_successful_fetch_time = 0;

// Cached data for fallback
static tfnsw_dual_departures_t cached_dual_departures = {0};
static bool has_cached_data = false;

// Debug info tracking
static tfnsw_debug_info_t debug_info = {0};

// Forward declaration
static int64_t get_current_time_ms(void);

// ============================================================================
// Quiet Hours Check (reduced fetching between 01:00 and 04:00)
// ============================================================================

static bool is_quiet_hours(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Check if time is synced (year > 2020)
    if (timeinfo.tm_year < 120) {
        return false;  // Time not synced, allow fetch
    }

    int hour = timeinfo.tm_hour;
    // Quiet hours: 01:00 to 04:00 (1, 2, 3)
    return (hour >= 1 && hour < 4);
}

// Check if we should allow a fetch during quiet hours (once every 5 min)
static bool should_fetch_during_quiet_hours(void) {
    static int64_t last_quiet_fetch = 0;
    int64_t now = get_current_time_ms();

    // Allow one fetch every 5 minutes during quiet hours
    if (now - last_quiet_fetch >= 300000) {
        last_quiet_fetch = now;
        return true;
    }
    return false;
}

// Calculate data staleness
static void update_data_staleness(tfnsw_dual_departures_t *deps) {
    if (!deps || deps->last_fetch_time == 0) {
        deps->is_stale = true;
        deps->data_age_seconds = 0;
        return;
    }

    int64_t now = get_current_time_ms();
    int64_t age_ms = now - deps->last_fetch_time;
    deps->data_age_seconds = (int)(age_ms / 1000);
    deps->is_stale = (age_ms > STALE_DATA_THRESHOLD_MS);
}

// Check if time is synced (for startup robustness)
static bool is_time_synced(void) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    return (tm_now->tm_year + 1900 >= 2024);  // More lenient check
}

// ============================================================================
// HTTP Event Handler
// ============================================================================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    // Handle both chunked and non-chunked responses
    if (http_buffer) {
      if (http_buffer_len + evt->data_len < HTTP_BUFFER_SIZE - 1) {
        memcpy(http_buffer + http_buffer_len, evt->data, evt->data_len);
        http_buffer_len += evt->data_len;
        http_buffer[http_buffer_len] = '\0';
      } else {
        // Buffer overflow - log warning and set flag
        if (!http_buffer_overflow) {
          ESP_LOGW(TAG, "HTTP buffer overflow! Buffer: %d, trying to add: %d",
                   http_buffer_len, evt->data_len);
          http_buffer_overflow = true;
        }
      }
    }
    break;
  default:
    break;
  }
  return ESP_OK;
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t tfnsw_init(void) {
  if (initialized) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Initializing TfNSW client");

  // Create mutex for thread-safe access
  data_mutex = xSemaphoreCreateMutex();
  if (!data_mutex) {
    ESP_LOGE(TAG, "Failed to create mutex");
    return ESP_ERR_NO_MEM;
  }

  // Allocate HTTP buffer - ESP32-C6 has no SPIRAM, use internal RAM
  ESP_LOGI(TAG, "Allocating HTTP buffer: %d bytes", HTTP_BUFFER_SIZE);
  http_buffer = heap_caps_malloc(HTTP_BUFFER_SIZE, MALLOC_CAP_8BIT);
  if (!http_buffer) {
    // Try with default malloc
    ESP_LOGW(TAG, "heap_caps_malloc failed, trying malloc");
    http_buffer = malloc(HTTP_BUFFER_SIZE);
  }
  if (!http_buffer) {
    ESP_LOGE(TAG, "Failed to allocate HTTP buffer (%d bytes)", HTTP_BUFFER_SIZE);
    vSemaphoreDelete(data_mutex);
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "HTTP buffer allocated at %p", http_buffer);
  memset(http_buffer, 0, HTTP_BUFFER_SIZE);  // Initialize to zero

  // Load API key from NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(TFNSW_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    size_t key_len = sizeof(api_key);
    err = nvs_get_str(nvs_handle, TFNSW_NVS_KEY_API, api_key, &key_len);
    if (err != ESP_OK) {
      api_key[0] = '\0';
      ESP_LOGW(TAG, "No API key stored in NVS");
    } else {
      ESP_LOGI(TAG, "API key loaded from NVS (length: %d)", strlen(api_key));
    }
    nvs_close(nvs_handle);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "NVS namespace not found - first run");
  } else {
    ESP_LOGE(TAG, "Failed to open NVS for reading: %s", esp_err_to_name(err));
  }

  // If no API key in NVS, or NVS key looks truncated, use default from config
  size_t default_len = strlen(TFNSW_DEFAULT_API_KEY);
  size_t nvs_len = strlen(api_key);

  if (default_len > 0 && (api_key[0] == '\0' || (nvs_len < default_len && nvs_len < 150))) {
    // NVS key missing or suspiciously short (likely truncated from old 128-byte buffer)
    if (nvs_len > 0 && nvs_len < default_len) {
      ESP_LOGW(TAG, "NVS key looks truncated (%d chars), using config default", nvs_len);
    }
    strncpy(api_key, TFNSW_DEFAULT_API_KEY, sizeof(api_key) - 1);
    api_key[sizeof(api_key) - 1] = '\0';
    ESP_LOGI(TAG, "Using default API key from config (length: %d)",
             strlen(api_key));
  }

  // Initialize departures structure
  memset(&current_departures, 0, sizeof(current_departures));
  current_departures.status = TFNSW_STATUS_IDLE;

  initialized = true;
  ESP_LOGI(TAG, "TfNSW client initialized");
  return ESP_OK;
}

void tfnsw_deinit(void) {
  if (!initialized)
    return;

  tfnsw_stop_background_fetch();

  if (http_buffer) {
    free(http_buffer);
    http_buffer = NULL;
  }

  if (data_mutex) {
    vSemaphoreDelete(data_mutex);
    data_mutex = NULL;
  }

  initialized = false;
  ESP_LOGI(TAG, "TfNSW client deinitialized");
}

// ============================================================================
// API Key Management
// ============================================================================

esp_err_t tfnsw_set_api_key(const char *key) {
  if (!key || strlen(key) == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  strncpy(api_key, key, sizeof(api_key) - 1);
  api_key[sizeof(api_key) - 1] = '\0';

  // Store in NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(TFNSW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return err;
  }

  err = nvs_set_str(nvs_handle, TFNSW_NVS_KEY_API, api_key);
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "API key saved to NVS");
  } else {
    ESP_LOGE(TAG, "Failed to save API key: %s", esp_err_to_name(err));
  }

  return err;
}

bool tfnsw_has_api_key(void) { return api_key[0] != '\0'; }

esp_err_t tfnsw_clear_api_key(void) {
  api_key[0] = '\0';

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open(TFNSW_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    nvs_erase_key(nvs_handle, TFNSW_NVS_KEY_API);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }

  ESP_LOGI(TAG, "API key cleared");
  return ESP_OK;
}

// ============================================================================
// Time Utilities
// ============================================================================

static int64_t get_current_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int tfnsw_calc_minutes_until(int64_t departure_time) {
  int64_t now = get_current_time_ms() / 1000;
  int diff_seconds = (int)(departure_time - now);
  return diff_seconds / 60;
}

// ============================================================================
// JSON Parsing
// ============================================================================

static int64_t parse_iso_time(const char *time_str) {
  // Parse ISO 8601 format: "2024-12-05T07:42:00+11:00"
  // The API returns Sydney local time with timezone offset
  // Since ESP32 is set to Sydney timezone, we just parse the local time components
  if (!time_str)
    return 0;

  struct tm tm = {0};
  int year, month, day, hour, min, sec;

  // Parse date/time components (ignore timezone since ESP is already set to Sydney)
  int parsed = sscanf(time_str, "%d-%d-%dT%d:%d:%d", &year, &month, &day,
                      &hour, &min, &sec);

  if (parsed < 6) {
    ESP_LOGW(TAG, "Failed to parse time: %s (parsed %d fields)", time_str, parsed);
    return 0;
  }

  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;  // Let mktime determine DST

  // mktime interprets tm as local time (Sydney) and returns UTC epoch
  time_t epoch = mktime(&tm);

  if (epoch == -1) {
    ESP_LOGW(TAG, "mktime failed for: %s", time_str);
    return 0;
  }

  return (int64_t)epoch;
}

// Sydney Metro City & Southwest line - complete station list (north to south)
// Index 0 = Tallawong (northern terminus), Index 19 = Sydenham (southern terminus)
static const char *metro_stations[] = {
    "Tallawong",           // 0
    "Rouse Hill",          // 1
    "Kellyville",          // 2
    "Bella Vista",         // 3
    "Hills Showground",    // 4 (also known as Showground)
    "Castle Hill",         // 5
    "Cherrybrook",         // 6
    "Epping",              // 7
    "Macquarie University",// 8
    "Macquarie Park",      // 9
    "North Ryde",          // 10
    "Chatswood",           // 11
    "Crows Nest",          // 12
    "Victoria Cross",      // 13 <-- Current station
    "Barangaroo",          // 14
    "Martin Place",        // 15
    "Gadigal",             // 16
    "Central",             // 17
    "Waterloo",            // 18
    "Sydenham",            // 19
    NULL
};

#define METRO_STATION_COUNT 20
#define VICTORIA_CROSS_INDEX 13

// Find station index by name (partial match supported)
static int find_station_index(const char *name) {
  if (!name) return -1;
  for (int i = 0; i < METRO_STATION_COUNT; i++) {
    if (strstr(name, metro_stations[i]) != NULL) {
      return i;
    }
  }
  // Also check for "Showground" variant
  if (strstr(name, "Showground") != NULL) {
    return 4; // Hills Showground
  }
  return -1;
}

tfnsw_direction_t
tfnsw_get_direction_from_destination(const char *destination) {
  if (!destination)
    return TFNSW_DIRECTION_UNKNOWN;

  int dest_index = find_station_index(destination);
  if (dest_index < 0) {
    return TFNSW_DIRECTION_UNKNOWN;
  }

  // Northbound = stations with index < Victoria Cross (13)
  // Southbound = stations with index > Victoria Cross (13)
  if (dest_index < VICTORIA_CROSS_INDEX) {
    return TFNSW_DIRECTION_NORTHBOUND;
  } else if (dest_index > VICTORIA_CROSS_INDEX) {
    return TFNSW_DIRECTION_SOUTHBOUND;
  }

  return TFNSW_DIRECTION_UNKNOWN;
}

// Build calling stations list dynamically based on destination
static void populate_calling_stations(tfnsw_departure_t *dep) {
  dep->calling_stations[0] = '\0';
  dep->direction = tfnsw_get_direction_from_destination(dep->destination);

  int dest_index = find_station_index(dep->destination);
  if (dest_index < 0) {
    // Destination not found, use fallback
    if (dep->direction == TFNSW_DIRECTION_NORTHBOUND) {
      strncpy(dep->calling_stations,
              "Crows Nest, Chatswood, North Ryde, Macquarie Park, "
              "Macquarie University, Epping, Cherrybrook, Castle Hill",
              sizeof(dep->calling_stations) - 1);
    } else if (dep->direction == TFNSW_DIRECTION_SOUTHBOUND) {
      strncpy(dep->calling_stations,
              "Barangaroo, Martin Place, Gadigal, Central, Waterloo",
              sizeof(dep->calling_stations) - 1);
    }
    return;
  }

  // Build dynamic list of calling stations between Victoria Cross and destination
  char *buf = dep->calling_stations;
  size_t buf_size = sizeof(dep->calling_stations);
  size_t written = 0;
  bool first = true;

  if (dep->direction == TFNSW_DIRECTION_NORTHBOUND) {
    // Northbound: iterate from Victoria Cross - 1 down to destination + 1
    // (excluding destination itself)
    for (int i = VICTORIA_CROSS_INDEX - 1; i > dest_index && written < buf_size - 1; i--) {
      if (!first) {
        int n = snprintf(buf + written, buf_size - written, ", ");
        if (n > 0) written += n;
      }
      int n = snprintf(buf + written, buf_size - written, "%s", metro_stations[i]);
      if (n > 0) written += n;
      first = false;
    }
  } else if (dep->direction == TFNSW_DIRECTION_SOUTHBOUND) {
    // Southbound: iterate from Victoria Cross + 1 up to destination - 1
    // (excluding destination itself)
    for (int i = VICTORIA_CROSS_INDEX + 1; i < dest_index && written < buf_size - 1; i++) {
      if (!first) {
        int n = snprintf(buf + written, buf_size - written, ", ");
        if (n > 0) written += n;
      }
      int n = snprintf(buf + written, buf_size - written, "%s", metro_stations[i]);
      if (n > 0) written += n;
      first = false;
    }
  }
}

static void parse_departure(cJSON *stop_event, tfnsw_departure_t *dep) {
  memset(dep, 0, sizeof(tfnsw_departure_t));

  // Transportation info (line, destination)
  cJSON *transport = cJSON_GetObjectItem(stop_event, "transportation");
  if (transport) {
    cJSON *dest = cJSON_GetObjectItem(transport, "destination");
    if (dest) {
      cJSON *name = cJSON_GetObjectItem(dest, "name");
      if (name && cJSON_IsString(name)) {
        strncpy(dep->destination, name->valuestring,
                sizeof(dep->destination) - 1);
      }
    }

    cJSON *number = cJSON_GetObjectItem(transport, "number");
    if (number && cJSON_IsString(number)) {
      strncpy(dep->line_name, number->valuestring, sizeof(dep->line_name) - 1);
    }

    cJSON *product = cJSON_GetObjectItem(transport, "product");
    if (product) {
      cJSON *pname = cJSON_GetObjectItem(product, "name");
      if (pname && cJSON_IsString(pname)) {
        // Use product name if line name is empty
        if (dep->line_name[0] == '\0') {
          strncpy(dep->line_name, pname->valuestring,
                  sizeof(dep->line_name) - 1);
        }
      }
    }
  }

  // Scheduled departure time
  cJSON *planned = cJSON_GetObjectItem(stop_event, "departureTimePlanned");
  if (planned && cJSON_IsString(planned)) {
    dep->scheduled_time = parse_iso_time(planned->valuestring);
  }

  // Real-time estimated departure time
  cJSON *estimated = cJSON_GetObjectItem(stop_event, "departureTimeEstimated");
  if (estimated && cJSON_IsString(estimated)) {
    dep->estimated_time = parse_iso_time(estimated->valuestring);
    dep->is_realtime = true;
  }

  // Calculate minutes and delay
  int64_t departure =
      dep->is_realtime ? dep->estimated_time : dep->scheduled_time;
  dep->mins_to_departure = tfnsw_calc_minutes_until(departure);

  if (dep->is_realtime && dep->scheduled_time > 0) {
    dep->delay_seconds = (int)(dep->estimated_time - dep->scheduled_time);
    dep->is_delayed = dep->delay_seconds > 60; // More than 1 minute delay
  }

  // Platform
  cJSON *location = cJSON_GetObjectItem(stop_event, "location");
  if (location) {
    cJSON *platform = cJSON_GetObjectItem(location, "platform");
    if (platform) {
      cJSON *pname = cJSON_GetObjectItem(platform, "name");
      if (pname && cJSON_IsString(pname)) {
        strncpy(dep->platform, pname->valuestring, sizeof(dep->platform) - 1);
      }
    }
  }

  // Check for cancellation
  cJSON *is_cancelled = cJSON_GetObjectItem(stop_event, "isCancelled");
  if (is_cancelled && cJSON_IsBool(is_cancelled)) {
    dep->is_cancelled = cJSON_IsTrue(is_cancelled);
  }

  // Realtime controlled
  cJSON *realtime = cJSON_GetObjectItem(stop_event, "isRealtimeControlled");
  if (realtime && cJSON_IsBool(realtime)) {
    dep->is_realtime = cJSON_IsTrue(realtime);
  }

  // Occupancy/loading
  cJSON *hints = cJSON_GetObjectItem(stop_event, "hints");
  if (hints && cJSON_IsObject(hints)) {
    cJSON *occupancy = cJSON_GetObjectItem(hints, "occupancy");
    if (occupancy && cJSON_IsString(occupancy)) {
      dep->occupancy_available = true;
      if (strcmp(occupancy->valuestring, "LOW") == 0) {
        dep->occupancy_percent = 25;
      } else if (strcmp(occupancy->valuestring, "MEDIUM") == 0) {
        dep->occupancy_percent = 50;
      } else if (strcmp(occupancy->valuestring, "HIGH") == 0) {
        dep->occupancy_percent = 75;
      } else if (strcmp(occupancy->valuestring, "VERY_HIGH") == 0) {
        dep->occupancy_percent = 95;
      }
    }
  }

  // Populate calling stations based on destination
  populate_calling_stations(dep);
}

static esp_err_t parse_response(const char *json_str,
                                tfnsw_departures_t *deps) {
  if (!json_str || json_str[0] == '\0') {
    ESP_LOGE(TAG, "Empty or null JSON string");
    return ESP_ERR_INVALID_RESPONSE;
  }

  int json_len = (int)strlen(json_str);
  int free_heap = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
  ESP_LOGI(TAG, "Parsing JSON: %d bytes, free heap: %d bytes", json_len, free_heap);

  // Update debug info
  debug_info.last_response_size = json_len;
  debug_info.last_parse_heap_before = free_heap;
  debug_info.buffer_size = HTTP_BUFFER_SIZE;
  debug_info.buffer_overflow = http_buffer_overflow;
  debug_info.fetch_count++;

  // Capture response start/end for debugging
  int start_len = json_len < 60 ? json_len : 60;
  strncpy(debug_info.response_start, json_str, start_len);
  debug_info.response_start[start_len] = '\0';
  if (json_len > 60) {
    strncpy(debug_info.response_end, json_str + json_len - 60, 60);
    debug_info.response_end[60] = '\0';
  } else {
    strcpy(debug_info.response_end, debug_info.response_start);
  }

  // Log first 100 chars to diagnose malformed responses
  ESP_LOGD(TAG, "Response start: %.100s", json_str);

  // Sanity check - valid JSON should start with { or [
  if (json_str[0] != '{' && json_str[0] != '[') {
    ESP_LOGE(TAG, "Invalid JSON - doesn't start with { or [. First char: 0x%02X '%c'",
             (unsigned char)json_str[0], json_str[0] > 31 ? json_str[0] : '?');
    ESP_LOGE(TAG, "First 50 chars: %.50s", json_str);
    debug_info.parse_fail_count++;
    snprintf(debug_info.parse_error_context, sizeof(debug_info.parse_error_context),
             "Invalid start char: 0x%02X", (unsigned char)json_str[0]);
    return ESP_ERR_INVALID_RESPONSE;
  }

  cJSON *root = cJSON_Parse(json_str);
  debug_info.last_parse_heap_after = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);

  if (!root) {
    const char *error_ptr = cJSON_GetErrorPtr();
    ESP_LOGE(TAG, "Failed to parse JSON (heap: %d, json: %d bytes)",
             debug_info.last_parse_heap_after, json_len);
    debug_info.parse_fail_count++;

    if (error_ptr) {
      // Calculate offset of error in the string
      int error_offset = (int)(error_ptr - json_str);
      debug_info.parse_error_offset = error_offset;
      ESP_LOGE(TAG, "Parse error at offset %d, near: %.50s", error_offset, error_ptr);
      snprintf(debug_info.parse_error_context, sizeof(debug_info.parse_error_context),
               "offset %d: %.40s", error_offset, error_ptr);
    } else {
      debug_info.parse_error_offset = -1;
      snprintf(debug_info.parse_error_context, sizeof(debug_info.parse_error_context),
               "cJSON_Parse returned NULL (heap exhausted?)");
    }
    // Log last 50 chars to see if response was truncated
    if (json_len > 50) {
      ESP_LOGE(TAG, "Response end: %.50s", json_str + json_len - 50);
    }
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Parse succeeded
  debug_info.parse_success_count++;
  debug_info.parse_error_offset = 0;
  debug_info.parse_error_context[0] = '\0';

  // Check for error response
  cJSON *error = cJSON_GetObjectItem(root, "error");
  if (error) {
    cJSON *message = cJSON_GetObjectItem(error, "message");
    if (message && cJSON_IsString(message)) {
      strncpy(deps->error_message, message->valuestring,
              sizeof(deps->error_message) - 1);
    }
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Get stop events array
  cJSON *stop_events = cJSON_GetObjectItem(root, "stopEvents");
  if (!stop_events || !cJSON_IsArray(stop_events)) {
    ESP_LOGW(TAG, "No stopEvents in response");
    deps->count = 0;
    deps->status = TFNSW_STATUS_ERROR_NO_DATA;
    strncpy(deps->error_message, "No departures found",
            sizeof(deps->error_message) - 1);
    cJSON_Delete(root);
    return ESP_OK; // Not an error, just no data
  }

  // Parse each departure
  int count = cJSON_GetArraySize(stop_events);
  deps->count = 0;

  for (int i = 0; i < count && deps->count < TFNSW_MAX_DEPARTURES; i++) {
    cJSON *event = cJSON_GetArrayItem(stop_events, i);
    if (!event)
      continue;

    tfnsw_departure_t *dep = &deps->departures[deps->count];
    parse_departure(event, dep);

    // Skip cancelled services and past departures
    if (dep->is_cancelled) {
      ESP_LOGD(TAG, "Skipping cancelled service to %s", dep->destination);
      continue;
    }
    if (dep->mins_to_departure < -1) {
      ESP_LOGD(TAG, "Skipping past departure to %s (%d min ago)",
               dep->destination, -dep->mins_to_departure);
      continue;
    }

    deps->count++;
  }

  // Get station name from first event's location
  if (count > 0) {
    cJSON *first_event = cJSON_GetArrayItem(stop_events, 0);
    if (first_event) {
      cJSON *location = cJSON_GetObjectItem(first_event, "location");
      if (location) {
        cJSON *name = cJSON_GetObjectItem(location, "name");
        if (name && cJSON_IsString(name)) {
          strncpy(deps->station_name, name->valuestring,
                  sizeof(deps->station_name) - 1);
        }
      }
    }
  }

  // Check for system messages / service suspensions
  cJSON *system_messages = cJSON_GetObjectItem(root, "systemMessages");
  if (system_messages && cJSON_IsArray(system_messages)) {
    int msg_count = cJSON_GetArraySize(system_messages);
    for (int i = 0; i < msg_count; i++) {
      cJSON *msg = cJSON_GetArrayItem(system_messages, i);
      if (msg) {
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        cJSON *text = cJSON_GetObjectItem(msg, "text");
        if (type && cJSON_IsString(type) &&
            (strcmp(type->valuestring, "error") == 0 ||
             strcmp(type->valuestring, "warning") == 0)) {
          if (text && cJSON_IsString(text)) {
            strncpy(deps->suspension_message, text->valuestring,
                    sizeof(deps->suspension_message) - 1);
            deps->service_suspended = (deps->count == 0);
          }
        }
      }
    }
  }

  cJSON_Delete(root);

  if (deps->count == 0 && !deps->service_suspended) {
    deps->status = TFNSW_STATUS_ERROR_NO_DATA;
    strncpy(deps->error_message, "No upcoming services",
            sizeof(deps->error_message) - 1);
  } else {
    deps->status = TFNSW_STATUS_SUCCESS;
  }

  ESP_LOGI(TAG, "Parsed %d departures from %s", deps->count,
           deps->station_name);
  return ESP_OK;
}

// ============================================================================
// HTTP Fetch
// ============================================================================

esp_err_t tfnsw_fetch_departures(const char *stop_id,
                                 tfnsw_departures_t *out_departures) {
  if (!initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!tfnsw_has_api_key()) {
    out_departures->status = TFNSW_STATUS_ERROR_NO_API_KEY;
    strncpy(out_departures->error_message, "API key required",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_INVALID_STATE;
  }

  // Build URL with query parameters
  char url[512];
  time_t now = time(NULL);
  struct tm *tm_now = localtime(&now);

  // Validate time is synced (be lenient - allow 2024 or later)
  if (tm_now->tm_year + 1900 < 2024) {
    ESP_LOGW(TAG, "Time not synced yet (year=%d), attempting fetch anyway", tm_now->tm_year + 1900);
    // Don't block - attempt fetch but warn user
    out_departures->status = TFNSW_STATUS_ERROR_TIME_NOT_SYNCED;
    strncpy(out_departures->error_message, "Time sync pending",
            sizeof(out_departures->error_message) - 1);
    // Continue anyway - the API will work, just timestamps may be off
  }

  // Detect stop type: train stops start with "101", metro with "206"
  bool is_train_stop = (strncmp(stop_id, "101", 3) == 0);

  if (is_train_stop) {
    // Train station - include trains (MOT_1), exclude metro and others
    // Use smaller limit (3) to reduce response size and memory usage
    snprintf(url, sizeof(url),
             "%s%s?"
             "outputFormat=rapidJSON"
             "&coordOutputFormat=EPSG:4326"
             "&mode=direct"
             "&type_dm=stop"
             "&name_dm=%s"
             "&depArrMacro=dep"
             "&itdDate=%04d%02d%02d"
             "&itdTime=%02d%02d"
             "&TfNSWDM=true"
             "&version=10.2.1.42"
             "&excludedMeans=checkbox"
             "&exclMOT_2=1"   // Exclude metro
             "&exclMOT_4=1"   // Exclude light rail
             "&exclMOT_5=1"   // Exclude buses
             "&exclMOT_7=1"   // Exclude coaches
             "&exclMOT_9=1"   // Exclude ferries
             "&exclMOT_11=1"  // Exclude school buses
             "&limit_dm=1",   // 1 train only - Sydney Trains returns ~16KB per departure!
             TFNSW_API_BASE_URL, TFNSW_API_DEPARTURE_PATH, stop_id,
             tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
             tm_now->tm_hour, tm_now->tm_min);
  } else {
    // Metro station - include metro (MOT_2), exclude trains and others
    snprintf(url, sizeof(url),
             "%s%s?"
             "outputFormat=rapidJSON"
             "&coordOutputFormat=EPSG:4326"
             "&mode=direct"
             "&type_dm=stop"
             "&name_dm=%s"
             "&depArrMacro=dep"
             "&itdDate=%04d%02d%02d"
             "&itdTime=%02d%02d"
             "&TfNSWDM=true"
             "&version=10.2.1.42"
             "&excludedMeans=checkbox"
             "&exclMOT_1=1"   // Exclude trains
             "&exclMOT_4=1"   // Exclude light rail
             "&exclMOT_5=1"   // Exclude buses
             "&exclMOT_7=1"   // Exclude coaches
             "&exclMOT_9=1"   // Exclude ferries
             "&exclMOT_11=1"  // Exclude school buses
             "&limit_dm=4",
             TFNSW_API_BASE_URL, TFNSW_API_DEPARTURE_PATH, stop_id,
             tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
             tm_now->tm_hour, tm_now->tm_min);
  }

  ESP_LOGI(TAG, "Fetching departures from: %s", url);

  // Build authorization header
  char auth_header[600]; // Enough for "apikey " + 512 byte JWT + null
  snprintf(auth_header, sizeof(auth_header), "apikey %s", api_key);

  // Reset buffer
  http_buffer_len = 0;
  http_buffer[0] = '\0';
  http_buffer_overflow = false;

  // Configure HTTP client with TLS
  esp_http_client_config_t config = {
      .url = url,
      .event_handler = http_event_handler,
      .timeout_ms = TFNSW_FETCH_TIMEOUT_MS,
      .crt_bundle_attach = esp_crt_bundle_attach, // Use bundle for TLS
      .buffer_size = 2048,
      .buffer_size_tx = 1024,
      .disable_auto_redirect = false,
      .keep_alive_enable = false,
      .skip_cert_common_name_check = true,  // Allow cert name mismatch
  };

  ESP_LOGI(TAG, "Connecting to: %s", url);

  ESP_LOGI(TAG, "Authorization header set (key length: %d)", strlen(api_key));

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
    strncpy(out_departures->error_message, "HTTP client init failed",
            sizeof(out_departures->error_message) - 1);
    return ESP_FAIL;
  }

  // Set authorization header
  esp_http_client_set_header(client, "Authorization", auth_header);
  esp_http_client_set_header(client, "Accept", "application/json");

  // Perform request
  out_departures->status = TFNSW_STATUS_FETCHING;
  esp_err_t err = esp_http_client_perform(client);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s (0x%x)", esp_err_to_name(err), err);
    ESP_LOGE(TAG, "URL was: %s", url);

    // More specific error classification
    if (err == ESP_ERR_HTTP_CONNECT) {
      out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
      strncpy(out_departures->error_message, "Connection failed",
              sizeof(out_departures->error_message) - 1);
    } else if (err == ESP_ERR_HTTP_WRITE_DATA || err == ESP_ERR_HTTP_FETCH_HEADER) {
      out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
      strncpy(out_departures->error_message, "Request failed",
              sizeof(out_departures->error_message) - 1);
    } else if (err == ESP_ERR_TIMEOUT) {
      out_departures->status = TFNSW_STATUS_ERROR_TIMEOUT;
      strncpy(out_departures->error_message, "Request timeout",
              sizeof(out_departures->error_message) - 1);
    } else {
      out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
      snprintf(out_departures->error_message,
               sizeof(out_departures->error_message), "Error: %s",
               esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
  }

  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP status: %d, response length: %d", status_code,
           http_buffer_len);

  esp_http_client_cleanup(client);

  // Handle HTTP status codes
  switch (status_code) {
  case 200:
    break; // Success, continue to parse
  case 401:
    out_departures->status = TFNSW_STATUS_ERROR_AUTH;
    strncpy(out_departures->error_message, "Invalid API key",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_INVALID_ARG;
  case 403:
    out_departures->status = TFNSW_STATUS_ERROR_RATE_LIMIT;
    strncpy(out_departures->error_message, "Rate limit exceeded",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_INVALID_STATE;
  case 404:
    out_departures->status = TFNSW_STATUS_ERROR_NO_DATA;
    strncpy(out_departures->error_message, "Stop not found",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_NOT_FOUND;
  default:
    if (status_code >= 500) {
      out_departures->status = TFNSW_STATUS_ERROR_SERVER;
      snprintf(out_departures->error_message,
               sizeof(out_departures->error_message), "Server error (%d)",
               status_code);
      return ESP_FAIL;
    }
    out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
    snprintf(out_departures->error_message,
             sizeof(out_departures->error_message), "HTTP error %d",
             status_code);
    return ESP_FAIL;
  }

  // Parse JSON response
  if (http_buffer_len == 0) {
    out_departures->status = TFNSW_STATUS_ERROR_PARSE;
    strncpy(out_departures->error_message, "Empty response",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Log warning for large responses approaching buffer limit
  if (http_buffer_len > HTTP_BUFFER_WARNING_THRESHOLD) {
    ESP_LOGW(TAG, "Large response: %d bytes (%.0f%% of buffer)",
             http_buffer_len, (float)http_buffer_len / HTTP_BUFFER_SIZE * 100);
  }

  if (http_buffer_overflow) {
    ESP_LOGE(TAG, "Response truncated: buffer overflow at %d bytes", HTTP_BUFFER_SIZE);
    out_departures->status = TFNSW_STATUS_ERROR_RESPONSE_TOO_LARGE;
    snprintf(out_departures->error_message, sizeof(out_departures->error_message),
             "Response too large (>%dKB)", HTTP_BUFFER_SIZE / 1024);
    return ESP_ERR_INVALID_SIZE;
  }

  ESP_LOGI(TAG, "Parsing %d bytes, free heap: %lu bytes", http_buffer_len, esp_get_free_heap_size());

  // Verify buffer integrity before parsing
  if (!http_buffer || http_buffer_len == 0) {
    ESP_LOGE(TAG, "HTTP buffer is null or empty");
    out_departures->status = TFNSW_STATUS_ERROR_PARSE;
    strncpy(out_departures->error_message, "Empty response buffer",
            sizeof(out_departures->error_message) - 1);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Memory optimization: shrink buffer to actual size before parsing
  // This frees up heap for cJSON which needs ~2x the JSON size
  int response_size = http_buffer_len;  // Save for error reporting
  int actual_size = http_buffer_len + 1;  // +1 for null terminator
  char *compact_buffer = realloc(http_buffer, actual_size);
  if (compact_buffer) {
    http_buffer = compact_buffer;
    ESP_LOGI(TAG, "Compacted buffer from %d to %d bytes, free heap: %lu",
             HTTP_BUFFER_SIZE, actual_size, esp_get_free_heap_size());
  }

  err = parse_response(http_buffer, out_departures);

  // Restore full buffer size for next request
  char *full_buffer = realloc(http_buffer, HTTP_BUFFER_SIZE);
  if (full_buffer) {
    http_buffer = full_buffer;
  } else {
    // If realloc fails, try fresh allocation
    free(http_buffer);
    http_buffer = heap_caps_malloc(HTTP_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!http_buffer) {
      http_buffer = malloc(HTTP_BUFFER_SIZE);
    }
  }
  http_buffer_len = 0;
  if (http_buffer) {
    http_buffer[0] = '\0';
  }

  // Return parse result (err was set above)
  if (err != ESP_OK) {
    out_departures->status = TFNSW_STATUS_ERROR_PARSE;
    if (out_departures->error_message[0] == '\0') {
      // Include response size in error for debugging
      snprintf(out_departures->error_message,
               sizeof(out_departures->error_message),
               "Parse failed (%d bytes)", response_size);
    }
    return err;
  }

  out_departures->last_fetch_time = get_current_time_ms();
  out_departures->consecutive_errors = 0;

  return ESP_OK;
}

esp_err_t tfnsw_fetch_victoria_cross(tfnsw_departures_t *out_departures) {
  return tfnsw_fetch_departures(TFNSW_VICTORIA_CROSS_STOP_ID, out_departures);
}

// Helper to sort departures by departure time (ascending)
static int compare_departures(const void *a, const void *b) {
  const tfnsw_departure_t *dep_a = (const tfnsw_departure_t *)a;
  const tfnsw_departure_t *dep_b = (const tfnsw_departure_t *)b;
  return dep_a->mins_to_departure - dep_b->mins_to_departure;
}

esp_err_t
tfnsw_fetch_victoria_cross_dual(tfnsw_dual_departures_t *out_departures) {
  if (!initialized || !out_departures) {
    return ESP_ERR_INVALID_STATE;
  }

  memset(out_departures, 0, sizeof(tfnsw_dual_departures_t));
  strncpy(out_departures->station_name, "Victoria Cross",
          sizeof(out_departures->station_name) - 1);

  // Fetch from the main stop ID which includes all directions
  tfnsw_departures_t all_deps;
  esp_err_t err =
      tfnsw_fetch_departures(TFNSW_VICTORIA_CROSS_STOP_ID, &all_deps);

  // Copy metadata
  out_departures->status = all_deps.status;
  out_departures->last_fetch_time = all_deps.last_fetch_time;
  out_departures->consecutive_errors = all_deps.consecutive_errors;
  strncpy(out_departures->error_message, all_deps.error_message,
          sizeof(out_departures->error_message) - 1);
  out_departures->service_suspended = all_deps.service_suspended;
  strncpy(out_departures->suspension_message, all_deps.suspension_message,
          sizeof(out_departures->suspension_message) - 1);

  if (err != ESP_OK || all_deps.status != TFNSW_STATUS_SUCCESS) {
    return err;
  }

  // Separate departures by direction
  for (int i = 0; i < all_deps.count; i++) {
    tfnsw_departure_t *dep = &all_deps.departures[i];

    if (dep->direction == TFNSW_DIRECTION_NORTHBOUND &&
        out_departures->northbound_count < TFNSW_MAX_PER_DIRECTION) {
      memcpy(&out_departures->northbound[out_departures->northbound_count], dep,
             sizeof(tfnsw_departure_t));
      out_departures->northbound_count++;
    } else if (dep->direction == TFNSW_DIRECTION_SOUTHBOUND &&
               out_departures->southbound_count < TFNSW_MAX_PER_DIRECTION) {
      memcpy(&out_departures->southbound[out_departures->southbound_count], dep,
             sizeof(tfnsw_departure_t));
      out_departures->southbound_count++;
    }
  }

  // Sort each direction by departure time
  if (out_departures->northbound_count > 1) {
    qsort(out_departures->northbound, out_departures->northbound_count,
          sizeof(tfnsw_departure_t), compare_departures);
  }
  if (out_departures->southbound_count > 1) {
    qsort(out_departures->southbound, out_departures->southbound_count,
          sizeof(tfnsw_departure_t), compare_departures);
  }

  ESP_LOGI(TAG, "Dual fetch: %d northbound, %d southbound",
           out_departures->northbound_count, out_departures->southbound_count);

  return ESP_OK;
}

// ============================================================================
// Background Fetch Task
// ============================================================================

static void fetch_task(void *arg) {
  ESP_LOGI(TAG, "Background fetch task started");

  TickType_t last_fetch = 0;
  int backoff_multiplier = 1;

  while (fetch_task_running) {
    TickType_t now = xTaskGetTickCount();
    TickType_t interval =
        pdMS_TO_TICKS(TFNSW_FETCH_INTERVAL_MS * backoff_multiplier);

    // Check if it's time to fetch or force refresh requested
    bool should_fetch = force_refresh_flag || (now - last_fetch >= interval) ||
                        (last_fetch == 0);

    // Skip fetching during quiet hours (01:00 - 04:00)
    if (is_quiet_hours()) {
      should_fetch = false;
    }

    if (should_fetch) {
      force_refresh_flag = false;
      last_fetch = now;

      tfnsw_departures_t new_data;
      memset(&new_data, 0, sizeof(new_data));

      esp_err_t err = tfnsw_fetch_victoria_cross(&new_data);

      // Update shared data with mutex
      if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(&current_departures, &new_data, sizeof(new_data));
        xSemaphoreGive(data_mutex);
      }

      // Handle backoff on errors
      if (err == ESP_OK && new_data.status == TFNSW_STATUS_SUCCESS) {
        backoff_multiplier = 1; // Reset backoff on success
      } else {
        current_departures.consecutive_errors++;
        // Exponential backoff: 1x, 2x, 4x, 8x max
        if (backoff_multiplier < 8) {
          backoff_multiplier *= 2;
        }
        ESP_LOGW(TAG, "Fetch failed, backoff multiplier: %d",
                 backoff_multiplier);
      }

      // Call update callback
      if (update_callback) {
        update_callback(&current_departures);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Check every second
  }

  ESP_LOGI(TAG, "Background fetch task stopped");
  vTaskDelete(NULL);
}

esp_err_t tfnsw_start_background_fetch(
    void (*on_update)(const tfnsw_departures_t *departures)) {
  if (!initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (fetch_task_running) {
    ESP_LOGW(TAG, "Background fetch already running");
    return ESP_OK;
  }

  update_callback = on_update;
  fetch_task_running = true;
  force_refresh_flag = true; // Fetch immediately on start

  BaseType_t ret = xTaskCreate(fetch_task, "tfnsw_fetch",
                               16384, // Large stack for mbedTLS/HTTPS
                               NULL,
                               5, // Medium priority
                               &fetch_task_handle);

  if (ret != pdPASS) {
    fetch_task_running = false;
    ESP_LOGE(TAG, "Failed to create fetch task");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Background fetch started");
  return ESP_OK;
}

void tfnsw_stop_background_fetch(void) {
  if (!fetch_task_running)
    return;

  fetch_task_running = false;
  update_callback = NULL;
  dual_update_callback = NULL;
  dual_mode_enabled = false;
  simple_mode_enabled = false;
  single_view_mode_enabled = false;
  single_view_callback = NULL;
  active_stop_id[0] = '\0';

  // Wait for task to finish
  vTaskDelay(pdMS_TO_TICKS(100));

  ESP_LOGI(TAG, "Background fetch stopped");
}

// ============================================================================
// Dual-Direction Background Fetch Task
// ============================================================================

static void dual_fetch_task(void *arg) {
  ESP_LOGI(TAG, "Dual-direction background fetch task started");

  TickType_t last_fetch = 0;
  int backoff_multiplier = 1;
  int retry_count = 0;

  while (fetch_task_running && dual_mode_enabled) {
    TickType_t now = xTaskGetTickCount();
    TickType_t interval =
        pdMS_TO_TICKS(TFNSW_FETCH_INTERVAL_MS * backoff_multiplier);

    // Check if it's time to fetch or force refresh requested
    bool should_fetch = force_refresh_flag || (now - last_fetch >= interval) ||
                        (last_fetch == 0);

    // During quiet hours, only fetch periodically (every 5 min instead of never)
    if (is_quiet_hours()) {
      if (!force_refresh_flag && !should_fetch_during_quiet_hours()) {
        should_fetch = false;
      }
    }

    if (should_fetch) {
      force_refresh_flag = false;
      last_fetch = now;

      tfnsw_dual_departures_t new_data;
      memset(&new_data, 0, sizeof(new_data));

      esp_err_t err = ESP_FAIL;

      // Retry loop for transient errors
      for (retry_count = 0; retry_count < MAX_HTTP_RETRIES; retry_count++) {
        if (retry_count > 0) {
          ESP_LOGI(TAG, "Retry attempt %d/%d", retry_count + 1, MAX_HTTP_RETRIES);
          vTaskDelay(pdMS_TO_TICKS(HTTP_RETRY_DELAY_MS * retry_count));
        }

        err = tfnsw_fetch_victoria_cross_dual(&new_data);

        // Don't retry for non-transient errors
        if (err == ESP_OK ||
            new_data.status == TFNSW_STATUS_ERROR_AUTH ||
            new_data.status == TFNSW_STATUS_ERROR_NO_API_KEY ||
            new_data.status == TFNSW_STATUS_ERROR_RATE_LIMIT) {
          break;
        }
      }

      // Handle fetch result
      if (err == ESP_OK && new_data.status == TFNSW_STATUS_SUCCESS) {
        // Success - cache the data and reset backoff
        backoff_multiplier = 1;
        new_data.consecutive_errors = 0;
        new_data.is_cached_fallback = false;
        last_successful_fetch_time = get_current_time_ms();

        // Update cache
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          memcpy(&cached_dual_departures, &new_data, sizeof(new_data));
          has_cached_data = true;
          memcpy(&current_dual_departures, &new_data, sizeof(new_data));
          xSemaphoreGive(data_mutex);
        }
      } else {
        // Error - use cached data as fallback if available
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (has_cached_data && cached_dual_departures.northbound_count > 0) {
            ESP_LOGW(TAG, "Fetch failed, using cached data as fallback");
            memcpy(&current_dual_departures, &cached_dual_departures, sizeof(cached_dual_departures));
            current_dual_departures.status = TFNSW_STATUS_SUCCESS_CACHED;
            current_dual_departures.is_cached_fallback = true;
            current_dual_departures.consecutive_errors++;
            // Truncate the original error message to fit within buffer
            char truncated_err[64];
            strncpy(truncated_err, new_data.error_message, sizeof(truncated_err) - 1);
            truncated_err[sizeof(truncated_err) - 1] = '\0';
            snprintf(current_dual_departures.error_message,
                     sizeof(current_dual_departures.error_message),
                     "Cached (%s)", truncated_err);
          } else {
            // No cache available - show error
            memcpy(&current_dual_departures, &new_data, sizeof(new_data));
            current_dual_departures.consecutive_errors++;
          }
          xSemaphoreGive(data_mutex);
        }

        // Exponential backoff: 1x, 2x, 4x, max 4x (don't go too slow)
        if (backoff_multiplier < 4) {
          backoff_multiplier *= 2;
        }
        ESP_LOGW(TAG, "Dual fetch failed after %d retries, backoff: %dx, error: %s",
                 retry_count, backoff_multiplier, new_data.error_message);
      }

      // Update staleness info
      update_data_staleness(&current_dual_departures);

      // Call update callback
      if (dual_update_callback) {
        dual_update_callback(&current_dual_departures);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500)); // Check every 500ms for responsiveness
  }

  ESP_LOGI(TAG, "Dual-direction background fetch task stopped");
  vTaskDelete(NULL);
}

esp_err_t tfnsw_start_dual_background_fetch(
    void (*on_update)(const tfnsw_dual_departures_t *departures)) {
  if (!initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (fetch_task_running) {
    ESP_LOGW(TAG, "Background fetch already running");
    return ESP_OK;
  }

  dual_update_callback = on_update;
  dual_mode_enabled = true;
  fetch_task_running = true;
  force_refresh_flag = true; // Fetch immediately on start

  BaseType_t ret = xTaskCreate(dual_fetch_task, "tfnsw_dual",
                               16384, // Large stack for mbedTLS/HTTPS
                               NULL,
                               5, // Medium priority
                               &fetch_task_handle);

  if (ret != pdPASS) {
    fetch_task_running = false;
    dual_mode_enabled = false;
    ESP_LOGE(TAG, "Failed to create dual fetch task");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Dual-direction background fetch started");
  return ESP_OK;
}

void tfnsw_force_refresh(void) { force_refresh_flag = true; }

bool tfnsw_is_fetching(void) {
  return fetch_task_running &&
         current_departures.status == TFNSW_STATUS_FETCHING;
}

bool tfnsw_is_background_fetch_running(void) { return fetch_task_running; }

// ============================================================================
// Data Access
// ============================================================================

void tfnsw_get_current_departures(tfnsw_departures_t *out_departures) {
  if (!out_departures)
    return;

  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    memcpy(out_departures, &current_departures, sizeof(tfnsw_departures_t));
    xSemaphoreGive(data_mutex);
  } else {
    memset(out_departures, 0, sizeof(tfnsw_departures_t));
    out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
  }
}

void tfnsw_get_current_dual_departures(
    tfnsw_dual_departures_t *out_departures) {
  if (!out_departures)
    return;

  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    memcpy(out_departures, &current_dual_departures,
           sizeof(tfnsw_dual_departures_t));
    xSemaphoreGive(data_mutex);
  } else {
    memset(out_departures, 0, sizeof(tfnsw_dual_departures_t));
    out_departures->status = TFNSW_STATUS_ERROR_NETWORK;
  }
}

tfnsw_status_t tfnsw_get_status(void) { return current_departures.status; }

const char *tfnsw_status_to_string(tfnsw_status_t status) {
  switch (status) {
  case TFNSW_STATUS_IDLE:
    return "Ready";
  case TFNSW_STATUS_FETCHING:
    return "Updating...";
  case TFNSW_STATUS_SUCCESS:
    return "Live";
  case TFNSW_STATUS_SUCCESS_CACHED:
    return "Cached";
  case TFNSW_STATUS_ERROR_NO_API_KEY:
    return "No API Key";
  case TFNSW_STATUS_ERROR_NETWORK:
    return "Network Error";
  case TFNSW_STATUS_ERROR_TIMEOUT:
    return "Timeout";
  case TFNSW_STATUS_ERROR_AUTH:
    return "Invalid Key";
  case TFNSW_STATUS_ERROR_RATE_LIMIT:
    return "Rate Limited";
  case TFNSW_STATUS_ERROR_SERVER:
    return "Server Error";
  case TFNSW_STATUS_ERROR_PARSE:
    return "Data Error";
  case TFNSW_STATUS_ERROR_NO_DATA:
    return "No Services";
  case TFNSW_STATUS_ERROR_RESPONSE_TOO_LARGE:
    return "Response Too Large";
  case TFNSW_STATUS_ERROR_TIME_NOT_SYNCED:
    return "Time Sync Pending";
  default:
    return "Unknown";
  }
}

// ============================================================================
// Formatting Utilities
// ============================================================================

void tfnsw_format_departure_time(int minutes, char *buffer,
                                 size_t buffer_size) {
  if (!buffer || buffer_size == 0)
    return;

  if (minutes <= 0) {
    snprintf(buffer, buffer_size, "NOW");
  } else if (minutes == 1) {
    snprintf(buffer, buffer_size, "1 min");
  } else if (minutes < 60) {
    snprintf(buffer, buffer_size, "%d min", minutes);
  } else {
    int hours = minutes / 60;
    int mins = minutes % 60;
    if (mins == 0) {
      snprintf(buffer, buffer_size, "%dh", hours);
    } else {
      snprintf(buffer, buffer_size, "%dh %dm", hours, mins);
    }
  }
}

void tfnsw_format_delay(int delay_seconds, char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0)
    return;

  int delay_mins = delay_seconds / 60;

  if (delay_mins == 0) {
    snprintf(buffer, buffer_size, "On time");
  } else if (delay_mins > 0) {
    snprintf(buffer, buffer_size, "+%d min", delay_mins);
  } else {
    snprintf(buffer, buffer_size, "%d min", delay_mins); // Already negative
  }
}

void tfnsw_get_debug_info(tfnsw_debug_info_t *out_info) {
  if (!out_info) return;
  memcpy(out_info, &debug_info, sizeof(tfnsw_debug_info_t));
}

// ============================================================================
// Simple Mode - Single Platform Fetch
// ============================================================================

void tfnsw_get_northbound_departures(tfnsw_departures_t *out_departures) {
  if (!out_departures) return;
  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
    memcpy(out_departures, &northbound_departures, sizeof(tfnsw_departures_t));
    xSemaphoreGive(data_mutex);
  }
}

void tfnsw_get_southbound_departures(tfnsw_departures_t *out_departures) {
  if (!out_departures) return;
  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
    memcpy(out_departures, &southbound_departures, sizeof(tfnsw_departures_t));
    xSemaphoreGive(data_mutex);
  }
}

void tfnsw_get_artarmon_departures(tfnsw_departures_t *out_departures) {
  if (!out_departures) return;
  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
    memcpy(out_departures, &artarmon_departures, sizeof(tfnsw_departures_t));
    xSemaphoreGive(data_mutex);
  }
}

static void simple_fetch_task(void *arg) {
  ESP_LOGI(TAG, "Simple background fetch task started");
  ESP_LOGI(TAG, "  - Victoria Cross Northbound: %s", TFNSW_VICTORIA_CROSS_NORTHBOUND);
  ESP_LOGI(TAG, "  - Crows Nest Southbound: %s", TFNSW_CROWS_NEST_SOUTHBOUND);
  ESP_LOGI(TAG, "  - Artarmon: %s", TFNSW_ARTARMON_STOP_ID);

  TickType_t last_fetch = 0;
  int backoff_multiplier = 1;

  while (fetch_task_running) {
    TickType_t now = xTaskGetTickCount();
    TickType_t interval = pdMS_TO_TICKS(TFNSW_FETCH_INTERVAL_MS * backoff_multiplier);

    // Check for forced refresh or interval elapsed
    bool should_fetch = force_refresh_flag || (now - last_fetch >= interval);

    if (should_fetch) {
      force_refresh_flag = false;
      is_currently_fetching = true;

      // Fetch Artarmon FIRST (Sydney Trains) - needs largest TLS buffer, best when heap is fresh
      if (artarmon_update_callback) {
        ESP_LOGI(TAG, "Fetching Artarmon departures (heap: %lu)...", esp_get_free_heap_size());
        tfnsw_departures_t artarmon_data = {0};
        esp_err_t artarmon_err = tfnsw_fetch_departures(TFNSW_ARTARMON_STOP_ID, &artarmon_data);

        strncpy(artarmon_data.station_name, "Artarmon", sizeof(artarmon_data.station_name) - 1);

        if (artarmon_err == ESP_OK && artarmon_data.status == TFNSW_STATUS_SUCCESS) {
          if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
            memcpy(&artarmon_departures, &artarmon_data, sizeof(tfnsw_departures_t));
            xSemaphoreGive(data_mutex);
          }
          ESP_LOGI(TAG, "Artarmon: %d departures", artarmon_data.count);
          backoff_multiplier = 1;
        } else {
          ESP_LOGW(TAG, "Artarmon fetch failed: %s (status=%d)", artarmon_data.error_message, artarmon_data.status);
        }

        // Always call callback so UI can show status/errors
        artarmon_update_callback(&artarmon_data);

        // Small delay between requests
        vTaskDelay(pdMS_TO_TICKS(500));
      }

      // Fetch northbound (Victoria Cross Platform 2)
      ESP_LOGI(TAG, "Fetching northbound departures...");
      tfnsw_departures_t north_data = {0};
      esp_err_t north_err = tfnsw_fetch_departures(TFNSW_VICTORIA_CROSS_NORTHBOUND, &north_data);

      strncpy(north_data.station_name, "Victoria Cross", sizeof(north_data.station_name) - 1);

      if (north_err == ESP_OK && north_data.status == TFNSW_STATUS_SUCCESS) {
        // Set direction for all departures
        for (int i = 0; i < north_data.count; i++) {
          north_data.departures[i].direction = TFNSW_DIRECTION_NORTHBOUND;
        }

        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
          memcpy(&northbound_departures, &north_data, sizeof(tfnsw_departures_t));
          xSemaphoreGive(data_mutex);
        }

        ESP_LOGI(TAG, "Northbound: %d departures", north_data.count);
      } else {
        ESP_LOGW(TAG, "Northbound fetch failed: %s (status=%d)", north_data.error_message, north_data.status);
      }

      // Always call callback so UI can show status/errors
      if (north_update_callback) {
        north_update_callback(&north_data);
      }

      // Small delay between requests to avoid rate limiting
      vTaskDelay(pdMS_TO_TICKS(500));

      // Fetch southbound (Crows Nest Platform 1)
      ESP_LOGI(TAG, "Fetching southbound departures...");
      tfnsw_departures_t south_data = {0};
      esp_err_t south_err = tfnsw_fetch_departures(TFNSW_CROWS_NEST_SOUTHBOUND, &south_data);

      strncpy(south_data.station_name, "Crows Nest", sizeof(south_data.station_name) - 1);

      if (south_err == ESP_OK && south_data.status == TFNSW_STATUS_SUCCESS) {
        // Set direction for all departures
        for (int i = 0; i < south_data.count; i++) {
          south_data.departures[i].direction = TFNSW_DIRECTION_SOUTHBOUND;
        }

        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
          memcpy(&southbound_departures, &south_data, sizeof(tfnsw_departures_t));
          xSemaphoreGive(data_mutex);
        }

        ESP_LOGI(TAG, "Southbound: %d departures", south_data.count);
      } else {
        ESP_LOGW(TAG, "Southbound fetch failed: %s (status=%d)", south_data.error_message, south_data.status);
        backoff_multiplier = backoff_multiplier < 4 ? backoff_multiplier + 1 : 4;
      }

      // Always call callback so UI can show status/errors
      if (south_update_callback) {
        south_update_callback(&south_data);
      }

      is_currently_fetching = false;
      last_fetch = xTaskGetTickCount();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));  // Check every second
  }

  ESP_LOGI(TAG, "Simple background fetch task stopped");
  fetch_task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t tfnsw_start_simple_background_fetch(
    void (*on_north_update)(const tfnsw_departures_t *departures),
    void (*on_south_update)(const tfnsw_departures_t *departures),
    void (*on_artarmon_update)(const tfnsw_departures_t *departures)) {

  if (!initialized) {
    ESP_LOGE(TAG, "TfNSW client not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (fetch_task_running) {
    ESP_LOGW(TAG, "Stopping existing fetch task");
    tfnsw_stop_background_fetch();
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  north_update_callback = on_north_update;
  south_update_callback = on_south_update;
  artarmon_update_callback = on_artarmon_update;
  simple_mode_enabled = true;
  dual_mode_enabled = false;
  fetch_task_running = true;
  force_refresh_flag = true;  // Fetch immediately

  BaseType_t ret = xTaskCreate(simple_fetch_task, "tfnsw_simple",
                               16384, NULL, 5, &fetch_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create simple fetch task");
    fetch_task_running = false;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Simple background fetch started");
  return ESP_OK;
}

// ============================================================================
// Single-View Mode - Only fetch for active view
// ============================================================================

static void single_view_fetch_task(void *arg) {
  ESP_LOGI(TAG, "Single-view background fetch task started");

  TickType_t last_fetch = 0;
  int backoff_multiplier = 1;

  while (fetch_task_running && single_view_mode_enabled) {
    TickType_t now = xTaskGetTickCount();
    TickType_t interval = pdMS_TO_TICKS(TFNSW_FETCH_INTERVAL_MS * backoff_multiplier);

    // Check for forced refresh or interval elapsed
    bool should_fetch = force_refresh_flag || (now - last_fetch >= interval);

    // Skip if no active stop
    if (active_stop_id[0] == '\0') {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // During quiet hours, fetch less frequently
    if (is_quiet_hours() && !force_refresh_flag) {
      if (!should_fetch_during_quiet_hours()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
    }

    if (should_fetch) {
      force_refresh_flag = false;
      is_currently_fetching = true;
      last_fetch = now;

      ESP_LOGI(TAG, "Fetching for stop: %s (heap: %lu)", active_stop_id, esp_get_free_heap_size());

      tfnsw_departures_t fetch_data = {0};
      esp_err_t err = tfnsw_fetch_departures(active_stop_id, &fetch_data);

      if (err == ESP_OK && fetch_data.status == TFNSW_STATUS_SUCCESS) {
        backoff_multiplier = 1;
        ESP_LOGI(TAG, "Fetch success: %d departures", fetch_data.count);
      } else {
        backoff_multiplier = backoff_multiplier < 4 ? backoff_multiplier * 2 : 4;
        ESP_LOGW(TAG, "Fetch failed: %s (backoff: %dx)", fetch_data.error_message, backoff_multiplier);
      }

      // Store and callback
      if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
        memcpy(&single_view_departures, &fetch_data, sizeof(tfnsw_departures_t));
        xSemaphoreGive(data_mutex);
      }

      is_currently_fetching = false;

      // Always call callback so UI can update status
      if (single_view_callback) {
        single_view_callback(&fetch_data);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }

  ESP_LOGI(TAG, "Single-view background fetch task stopped");
  fetch_task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t tfnsw_start_single_view_fetch(
    const char* stop_id,
    void (*on_update)(const tfnsw_departures_t* departures)) {

  if (!initialized) {
    ESP_LOGE(TAG, "TfNSW client not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  // Stop any existing fetch task
  if (fetch_task_running) {
    ESP_LOGI(TAG, "Stopping existing fetch task for single-view mode");
    tfnsw_stop_background_fetch();
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  // Set active stop
  if (stop_id && stop_id[0]) {
    strncpy(active_stop_id, stop_id, sizeof(active_stop_id) - 1);
    active_stop_id[sizeof(active_stop_id) - 1] = '\0';
  } else {
    active_stop_id[0] = '\0';
  }

  single_view_callback = on_update;
  single_view_mode_enabled = true;
  simple_mode_enabled = false;
  dual_mode_enabled = false;
  fetch_task_running = true;
  force_refresh_flag = true;

  // Clear any old data
  memset(&single_view_departures, 0, sizeof(single_view_departures));

  BaseType_t ret = xTaskCreate(single_view_fetch_task, "tfnsw_single",
                               16384, NULL, 5, &fetch_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create single-view fetch task");
    fetch_task_running = false;
    single_view_mode_enabled = false;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Single-view fetch started for stop: %s", stop_id ? stop_id : "(none)");
  return ESP_OK;
}

void tfnsw_set_active_stop(const char* stop_id) {
  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
    // Clear old data
    memset(&single_view_departures, 0, sizeof(single_view_departures));

    // Set new stop
    if (stop_id && stop_id[0]) {
      strncpy(active_stop_id, stop_id, sizeof(active_stop_id) - 1);
      active_stop_id[sizeof(active_stop_id) - 1] = '\0';
      ESP_LOGI(TAG, "Active stop changed to: %s", active_stop_id);
    } else {
      active_stop_id[0] = '\0';
      ESP_LOGI(TAG, "Active stop cleared (pausing fetch)");
    }

    xSemaphoreGive(data_mutex);
  }

  // Force immediate refresh for new stop
  force_refresh_flag = true;
}

void tfnsw_clear_cached_data(void) {
  if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100))) {
    memset(&single_view_departures, 0, sizeof(single_view_departures));
    memset(&northbound_departures, 0, sizeof(northbound_departures));
    memset(&southbound_departures, 0, sizeof(southbound_departures));
    memset(&artarmon_departures, 0, sizeof(artarmon_departures));
    memset(&current_departures, 0, sizeof(current_departures));
    memset(&current_dual_departures, 0, sizeof(current_dual_departures));
    xSemaphoreGive(data_mutex);
  }
  ESP_LOGI(TAG, "All cached data cleared");
}
