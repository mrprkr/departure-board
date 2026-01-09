#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"

#include "config.h"
#include "sd_card.h"

static const char *TAG = "sd_card";

static sdmmc_card_t *card = NULL;
static bool mounted = false;

esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");
    ESP_LOGI(TAG, "SD pins: CS=%d, MISO=%d (shared MOSI=%d, CLK=%d)",
             SD_PIN_CS, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CLK);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Note: SD card shares SPI bus with LCD, but uses different CS pin
    // The SPI bus should already be initialized by LCD driver with MISO pin

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;  // Use same SPI host as LCD

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting SD card at %s...", SD_MOUNT_POINT);
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount filesystem - no SD card or not FAT formatted");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed - SPI bus not initialized or already mounted");
        } else if (ret == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "Failed - not enough memory");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        }
        return ret;
    }

    mounted = true;

    // Print card info
    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(TAG, "SD card mounted successfully at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_card_deinit(void)
{
    if (!mounted) return ESP_OK;

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    card = NULL;
    mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return mounted;
}

uint64_t sd_card_get_total_bytes(void)
{
    if (!mounted || !card) return 0;

    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) {
        return 0;
    }

    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    return total_sectors * 512;
}

uint64_t sd_card_get_free_bytes(void)
{
    if (!mounted || !card) return 0;

    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) {
        return 0;
    }

    uint64_t free_sectors = free_clusters * fs->csize;
    return free_sectors * 512;
}

bool sd_file_exists(const char* path)
{
    if (!mounted) return false;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);

    struct stat st;
    return (stat(full_path, &st) == 0);
}

esp_err_t sd_read_file(const char* path, char* buffer, size_t buffer_size)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open file: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    size_t bytes_read = fread(buffer, 1, buffer_size - 1, f);
    buffer[bytes_read] = '\0';
    fclose(f);

    return ESP_OK;
}

esp_err_t sd_write_file(const char* path, const char* content)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to create file: %s", full_path);
        return ESP_FAIL;
    }

    fprintf(f, "%s", content);
    fclose(f);

    ESP_LOGI(TAG, "File written: %s", full_path);
    return ESP_OK;
}

esp_err_t sd_append_file(const char* path, const char* content)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);

    FILE *f = fopen(full_path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for append: %s", full_path);
        return ESP_FAIL;
    }

    fprintf(f, "%s", content);
    fclose(f);

    return ESP_OK;
}

esp_err_t sd_delete_file(const char* path)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "%s%s", SD_MOUNT_POINT, path);

    if (unlink(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File deleted: %s", full_path);
    return ESP_OK;
}
