/* ESP-IDF system headers */
#include "esp_log.h"     /* Logging framework for formatted terminal output (ESP_LOGI, etc.) */
#include "nvs_flash.h"   /* Flash storage required by Bluetooth for bonding / stack data */

/* Project modules */
#include "ble_app.h"   /* ble_app_init(): start NimBLE, advertising, and the UART service */
#include "console.h"   /* console_start(): start the USB-serial to BLE forwarder */

static const char *TAG = "MAIN";  /* Log tag so monitor output can be filtered to this module */

/* Initialize Non-Volatile Storage. Bluetooth will not start correctly without it. */
static void nvs_init(void)
{
    ESP_LOGI(TAG, "nvs_init(): calling nvs_flash_init()");
    esp_err_t ret = nvs_flash_init();  /* Map the NVS partition and prepare it for reads/writes */
    ESP_LOGI(TAG, "nvs_flash_init() returned 0x%x", ret);

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {  /* Partition empty, full, or format changed */
        ESP_LOGW(TAG, "NVS needs erase (no free pages or new version) — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());      /* Wipe the NVS partition; abort if erase fails */
        ESP_LOGI(TAG, "nvs_flash_erase() succeeded; calling nvs_flash_init() again");
        ret = nvs_flash_init();                  /* Retry init on the freshly erased partition */
        ESP_LOGI(TAG, "nvs_flash_init() retry returned 0x%x", ret);
    }

    ESP_ERROR_CHECK(ret);  /* Abort boot if NVS is still unusable */
    ESP_LOGI(TAG, "nvs_init(): NVS ready");
}

/*
 * Main application entry point.
 * ESP-IDF calls this once after the chip powers on and the RTOS has started.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "app_main(): ESP32 powered on — firmware starting");

    ESP_LOGI(TAG, "app_main(): initializing NVS (required by Bluetooth)");
    nvs_init();  /* Prepare flash storage before any BLE calls */

    ESP_LOGI(TAG, "app_main(): initializing BLE core and Nordic UART service");
    ble_app_init();  /* Start NimBLE, register UART GATT, spawn the host task */

    ESP_LOGI(TAG, "app_main(): starting USB-serial console task");
    console_start();  /* Spawn the task that reads typed lines and sends them over BLE */

    ESP_LOGI(TAG, "app_main(): setup complete — host and console tasks now run the application");
}
