#include "bsp.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp.fs";

static sdmmc_card_t *s_card;
static bool s_sd_mounted;

esp_err_t bsp_fs_mount(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path              = BSP_FS_MOUNT_POINT,
        .partition_label        = BSP_FS_PARTITION_LABEL,
        .format_if_mount_failed = true,   /* internal partition, ours to format */
        .dont_mount             = false,
    };

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(BSP_FS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs %s: %u/%u KB used", BSP_FS_MOUNT_POINT,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    return ESP_OK;
}

esp_err_t bsp_sd_mount(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    const spi_bus_config_t bus = {
        .sclk_io_num     = BSP_SD_PIN_SCLK,
        .mosi_io_num     = BSP_SD_PIN_MOSI,
        .miso_io_num     = BSP_SD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(BSP_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* already-inited is fine */
        ESP_LOGE(TAG, "sd spi bus init: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BSP_SD_SPI_HOST;
    /* 20 MHz is the SDSPI default and works on the CYD. Drop to 10000 if a
     * particular card throws timeouts — the slot has no series termination. */
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BSP_SD_PIN_CS;
    slot.host_id = host.slot;

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        /* Never true. This is the user's card and it may hold their own files. */
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        /* The CYD has no card-detect line, so "no card" and "bad card" both surface
         * here. Either way this is NOT fatal: the shell boots and reports no storage. */
        ESP_LOGW(TAG, "no usable microSD (%s)", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    s_sd_mounted = true;
    ESP_LOGI(TAG, "microSD mounted at %s: %s %lluMB", BSP_SD_MOUNT_POINT, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

bool bsp_sd_is_mounted(void)
{
    return s_sd_mounted;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_sd_mounted) {
        return ESP_OK;
    }
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
    s_sd_mounted = false;
    s_card = NULL;
    return err;
}
