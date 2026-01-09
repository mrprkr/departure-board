#ifndef TFNSW_CLIENT_H
#define TFNSW_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ============================================================================
// Configuration
// ============================================================================

// Victoria Cross Station stop IDs
// Platform 1 = Southbound (towards Sydenham)
// Platform 2 = Northbound (towards Tallawong)
#define TFNSW_VICTORIA_CROSS_STOP_ID     "206044"
#define TFNSW_VICTORIA_CROSS_SOUTHBOUND  "206047"  // Platform 1 - Sydenham
#define TFNSW_VICTORIA_CROSS_NORTHBOUND  "206046"  // Platform 2 - Tallawong

// Crows Nest Station stop IDs
// Platform 1 = Southbound (towards Sydenham)
// Platform 2 = Northbound (towards Tallawong)
#define TFNSW_CROWS_NEST_STOP_ID         "206034"
#define TFNSW_CROWS_NEST_SOUTHBOUND      "206037"  // Platform 1 - Sydenham
#define TFNSW_CROWS_NEST_NORTHBOUND      "206036"  // Platform 2 - Tallawong

// Artarmon Station stop ID (Sydney Trains)
#define TFNSW_ARTARMON_STOP_ID           "10101116"

// API configuration
#define TFNSW_API_BASE_URL      "https://api.transport.nsw.gov.au"
#define TFNSW_API_DEPARTURE_PATH "/v1/tp/departure_mon"
#define TFNSW_MAX_DEPARTURES    8       // Increased for dual-direction display
#define TFNSW_MAX_PER_DIRECTION 4       // Max departures per direction
#define TFNSW_FETCH_INTERVAL_MS 30000   // 30 seconds (max 2x per minute)
#define TFNSW_FETCH_TIMEOUT_MS  15000   // 15 second timeout
#define TFNSW_MAX_RETRIES       3

// Metro direction enumeration
typedef enum {
    TFNSW_DIRECTION_UNKNOWN = 0,
    TFNSW_DIRECTION_NORTHBOUND,  // Towards Tallawong
    TFNSW_DIRECTION_SOUTHBOUND,  // Towards Sydenham
} tfnsw_direction_t;

// ============================================================================
// Data Structures
// ============================================================================

// Connection/fetch status
typedef enum {
    TFNSW_STATUS_IDLE,
    TFNSW_STATUS_FETCHING,
    TFNSW_STATUS_SUCCESS,
    TFNSW_STATUS_SUCCESS_CACHED,    // Showing cached data due to error
    TFNSW_STATUS_ERROR_NO_API_KEY,
    TFNSW_STATUS_ERROR_NETWORK,
    TFNSW_STATUS_ERROR_TIMEOUT,
    TFNSW_STATUS_ERROR_AUTH,        // 401 - invalid API key
    TFNSW_STATUS_ERROR_RATE_LIMIT,  // 403 - rate/quota exceeded
    TFNSW_STATUS_ERROR_SERVER,      // 5xx errors
    TFNSW_STATUS_ERROR_PARSE,       // JSON parse error
    TFNSW_STATUS_ERROR_NO_DATA,     // No departures found
    TFNSW_STATUS_ERROR_RESPONSE_TOO_LARGE,  // Response exceeded buffer
    TFNSW_STATUS_ERROR_TIME_NOT_SYNCED,     // NTP not synced yet
} tfnsw_status_t;

// Service alert severity
typedef enum {
    TFNSW_ALERT_NONE = 0,
    TFNSW_ALERT_INFO,
    TFNSW_ALERT_WARNING,
    TFNSW_ALERT_SEVERE,
} tfnsw_alert_severity_t;

// Single departure information
typedef struct {
    char destination[64];       // Final destination name
    char platform[8];           // Platform number (if available)
    char line_name[32];         // Line/route name (e.g., "Metro North West")
    char calling_stations[128]; // Calling at stations (comma separated)

    // Times
    int64_t scheduled_time;     // Scheduled departure (Unix timestamp)
    int64_t estimated_time;     // Real-time estimated departure (0 if not available)
    int mins_to_departure;      // Minutes until departure
    int delay_seconds;          // Delay in seconds (negative = early)

    // Direction
    tfnsw_direction_t direction; // Which direction this service is heading

    // Flags
    bool is_realtime;           // Whether real-time data is available
    bool is_cancelled;          // Service is cancelled
    bool is_delayed;            // Service is delayed
    bool occupancy_available;   // Whether occupancy data is available
    uint8_t occupancy_percent;  // Carriage occupancy (0-100)

    // Alert
    tfnsw_alert_severity_t alert_severity;
    char alert_message[128];    // Short alert message if any
} tfnsw_departure_t;

// Collection of departures with metadata
typedef struct {
    tfnsw_departure_t departures[TFNSW_MAX_DEPARTURES];
    int count;                  // Number of valid departures

    // Station info
    char station_name[64];      // Full station name from API

    // Fetch metadata
    tfnsw_status_t status;      // Last fetch status
    int64_t last_fetch_time;    // When data was last fetched
    int64_t next_fetch_time;    // When to fetch next
    int consecutive_errors;     // Error counter for backoff
    char error_message[128];    // Human-readable error message

    // Data quality indicators
    bool is_stale;              // Data is older than 2 minutes
    bool is_cached_fallback;    // Showing cached data due to fetch error
    int data_age_seconds;       // How old the data is

    // Service status
    bool service_suspended;     // No services running
    char suspension_message[128];
} tfnsw_departures_t;

// Dual-direction departures (for Victoria Cross split display)
typedef struct {
    // Northbound (towards Tallawong)
    tfnsw_departure_t northbound[TFNSW_MAX_PER_DIRECTION];
    int northbound_count;

    // Southbound (towards Sydenham)
    tfnsw_departure_t southbound[TFNSW_MAX_PER_DIRECTION];
    int southbound_count;

    // Station info
    char station_name[64];

    // Fetch metadata
    tfnsw_status_t status;
    int64_t last_fetch_time;
    int consecutive_errors;
    char error_message[128];

    // Data quality indicators
    bool is_stale;              // Data is older than 2 minutes
    bool is_cached_fallback;    // Showing cached data due to fetch error
    int data_age_seconds;       // How old the data is

    // Service status
    bool service_suspended;
    char suspension_message[128];
} tfnsw_dual_departures_t;

// ============================================================================
// Initialization & Configuration
// ============================================================================

/**
 * Initialize the TfNSW client
 * Sets up HTTP client, loads API key from NVS
 */
esp_err_t tfnsw_init(void);

/**
 * Deinitialize and cleanup resources
 */
void tfnsw_deinit(void);

/**
 * Set the API key (stores in NVS)
 * @param api_key The TfNSW Open Data API key
 */
esp_err_t tfnsw_set_api_key(const char* api_key);

/**
 * Check if API key is configured
 */
bool tfnsw_has_api_key(void);

/**
 * Clear stored API key
 */
esp_err_t tfnsw_clear_api_key(void);

// ============================================================================
// Data Fetching
// ============================================================================

/**
 * Fetch departures synchronously (blocking)
 * @param stop_id The TfNSW stop ID (e.g., "206044" for Victoria Cross)
 * @param out_departures Pointer to departures structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tfnsw_fetch_departures(const char* stop_id, tfnsw_departures_t* out_departures);

/**
 * Fetch departures for Victoria Cross Station
 * Convenience wrapper for tfnsw_fetch_departures
 */
esp_err_t tfnsw_fetch_victoria_cross(tfnsw_departures_t* out_departures);

/**
 * Fetch departures for both directions at Victoria Cross
 * Makes parallel requests for northbound and southbound platforms
 * @param out_departures Dual-direction departures structure
 */
esp_err_t tfnsw_fetch_victoria_cross_dual(tfnsw_dual_departures_t* out_departures);

/**
 * Start background fetch task for dual-direction display
 * Fetches both directions at regular intervals
 */
esp_err_t tfnsw_start_dual_background_fetch(void (*on_update)(const tfnsw_dual_departures_t* departures));

/**
 * Start background fetch task
 * Fetches data at regular intervals and calls callback on update
 */
esp_err_t tfnsw_start_background_fetch(void (*on_update)(const tfnsw_departures_t* departures));

/**
 * Start simple background fetch for multiple stops
 * Fetches Victoria Cross Northbound, Crows Nest Southbound, and Artarmon
 * @param on_north_update Callback for northbound data updates
 * @param on_south_update Callback for southbound data updates
 * @param on_artarmon_update Callback for Artarmon data updates (can be NULL)
 */
esp_err_t tfnsw_start_simple_background_fetch(
    void (*on_north_update)(const tfnsw_departures_t* departures),
    void (*on_south_update)(const tfnsw_departures_t* departures),
    void (*on_artarmon_update)(const tfnsw_departures_t* departures));

/**
 * Start single-view background fetch (only fetches for active view)
 * More memory efficient - only one endpoint active at a time
 * @param stop_id The stop ID to fetch departures for
 * @param on_update Callback when data is updated
 */
esp_err_t tfnsw_start_single_view_fetch(
    const char* stop_id,
    void (*on_update)(const tfnsw_departures_t* departures));

/**
 * Switch the active stop for single-view fetch mode
 * Clears existing data and starts fetching for new stop
 * @param stop_id The new stop ID to fetch, or NULL to pause fetching
 */
void tfnsw_set_active_stop(const char* stop_id);

/**
 * Clear all cached departure data (call when switching views)
 */
void tfnsw_clear_cached_data(void);

/**
 * Get current northbound departures (Victoria Cross)
 */
void tfnsw_get_northbound_departures(tfnsw_departures_t* out_departures);

/**
 * Get current southbound departures (Crows Nest)
 */
void tfnsw_get_southbound_departures(tfnsw_departures_t* out_departures);

/**
 * Get current Artarmon departures (Sydney Trains)
 */
void tfnsw_get_artarmon_departures(tfnsw_departures_t* out_departures);

/**
 * Check if currently fetching
 */
bool tfnsw_is_fetching(void);

/**
 * Stop background fetch task
 */
void tfnsw_stop_background_fetch(void);

/**
 * Force an immediate refresh (resets fetch timer)
 */
void tfnsw_force_refresh(void);

/**
 * Check if currently fetching (status == FETCHING)
 */
bool tfnsw_is_fetching(void);

/**
 * Check if background fetch task is running
 */
bool tfnsw_is_background_fetch_running(void);

// ============================================================================
// Data Access
// ============================================================================

/**
 * Get the current departures data (thread-safe copy)
 * @param out_departures Pointer to departures structure to copy into
 */
void tfnsw_get_current_departures(tfnsw_departures_t* out_departures);

/**
 * Get the current dual-direction departures data (thread-safe copy)
 * @param out_departures Pointer to dual departures structure to copy into
 */
void tfnsw_get_current_dual_departures(tfnsw_dual_departures_t* out_departures);

/**
 * Determine direction from destination name
 * @param destination The destination station name
 * @return Direction enum (NORTHBOUND, SOUTHBOUND, or UNKNOWN)
 */
tfnsw_direction_t tfnsw_get_direction_from_destination(const char* destination);

/**
 * Get last fetch status
 */
tfnsw_status_t tfnsw_get_status(void);

/**
 * Get human-readable status string
 */
const char* tfnsw_status_to_string(tfnsw_status_t status);

/**
 * Calculate minutes until departure from timestamp
 */
int tfnsw_calc_minutes_until(int64_t departure_time);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Format departure time for display (e.g., "2 min", "NOW", "Due")
 * @param minutes Minutes until departure
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 */
void tfnsw_format_departure_time(int minutes, char* buffer, size_t buffer_size);

/**
 * Format delay for display (e.g., "+3 min", "-1 min", "On time")
 * @param delay_seconds Delay in seconds
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 */
void tfnsw_format_delay(int delay_seconds, char* buffer, size_t buffer_size);

// ============================================================================
// Debug Information
// ============================================================================

typedef struct {
    int last_response_size;         // Size of last HTTP response
    int last_parse_heap_before;     // Free heap before parsing
    int last_parse_heap_after;      // Free heap after parsing
    int parse_error_offset;         // Offset where parse error occurred
    char parse_error_context[64];   // Context around parse error
    char response_start[64];        // First 60 chars of response
    char response_end[64];          // Last 60 chars of response
    int fetch_count;                // Total fetch attempts
    int parse_success_count;        // Successful parses
    int parse_fail_count;           // Failed parses
    int buffer_size;                // Current buffer size
    bool buffer_overflow;           // Whether buffer overflowed
} tfnsw_debug_info_t;

/**
 * Get debug information about the last fetch/parse operation
 */
void tfnsw_get_debug_info(tfnsw_debug_info_t* out_info);

#endif // TFNSW_CLIENT_H
