/* Standard C libraries */
#include <assert.h>   /* Provides assert() to halt if GATT registration fails */
#include <string.h>   /* Provides strlen() for advertising name length */

/* ESP-IDF system headers */
#include "esp_log.h"              /* Logging framework for formatted terminal output (ESP_LOGI, etc.) */
#include "freertos/FreeRTOS.h"    /* Core FreeRTOS operating system structures */
#include "freertos/task.h"        /* FreeRTOS task types used by the NimBLE host task */

/* NimBLE stack core and porting layer */
#include "nimble/nimble_port.h"          /* Initializes and manages the overall NimBLE host stack */
#include "nimble/nimble_port_freertos.h" /* Runs the NimBLE stack inside a FreeRTOS background task */

/* NimBLE protocol-layer headers */
#include "host/ble_hs.h"    /* Host stack core: connection handles, callbacks, mbuf helpers */
#include "host/ble_gap.h"   /* GAP layer: radio state, connections, disconnections, advertising */
#include "host/ble_gatt.h"  /* GATT client helper used to request a larger MTU */
#include "host/ble_att.h"   /* ATT protocol: preferred MTU configuration */

/* Standard BLE profile services */
#include "services/gap/ble_svc_gap.h"    /* Standard GAP service (device name and appearance) */
#include "services/gatt/ble_svc_gatt.h"  /* Standard GATT service (attribute database updates) */

/* Project modules */
#include "ble_config.h"  /* Shared tunables: DEVICE_NAME, PREFERRED_MTU */
#include "ble_app.h"     /* Public BLE core API implemented in this file */
#include "ble_uart.h"    /* Nordic UART service registration and subscribe hooks */

static const char *TAG = "BLE_APP";  /* Log tag so monitor output can be filtered to this module */

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;  /* Active connection ID; NONE means disconnected */
static uint16_t current_mtu = 23;                       /* Negotiated MTU; BLE default is 23 until exchange completes */

static void ble_app_advertise(void);  /* Forward declaration so GAP events can restart advertising */

uint16_t ble_app_conn_handle(void)
{
    return conn_handle;  /* Hand the stored handle to UART send / other modules (NONE if disconnected) */
}

bool ble_app_is_connected(void)
{
    return conn_handle != BLE_HS_CONN_HANDLE_NONE;  /* True means a phone currently holds the link */
}

uint16_t ble_app_mtu(void)
{
    return current_mtu;  /* Return the negotiated MTU in bytes (default 23 until exchange completes) */
}

uint16_t ble_app_max_payload(void)
{
    if (current_mtu < 23) {     /* Guard against an invalid undersized MTU */
        return 20;              /* BLE default payload is 20 bytes */
    }
    return current_mtu - 3;     /* Subtract the 3-byte ATT header from the total MTU */
}

/*
 * GAP event handler.
 * NimBLE calls this whenever the radio reports connect, disconnect, MTU, subscribe, or adv complete.
 */
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;  /* Unused callback argument required by the NimBLE GAP signature */

    ESP_LOGI(TAG, "ble_gap_event(): type=%d", event->type);  /* Print which GAP event arrived */

    switch (event->type) {  /* Dispatch on the event type reported by the controller */
    case BLE_GAP_EVENT_CONNECT:  /* A connection attempt finished (success or failure) */
        ESP_LOGI(TAG, "GAP CONNECT: status=%d", event->connect.status);  /* 0 means the phone linked successfully */

        if (event->connect.status == 0) {                          /* Status 0 is a successful connection */
            conn_handle = event->connect.conn_handle;              /* Save the unique handle for later notifies */
            current_mtu = 23;                                      /* Reset MTU to the BLE default until exchange */
            ESP_LOGI(TAG, "Connected; handle=%d, MTU reset to %u", conn_handle, current_mtu);

            ble_uart_on_link_reset();                              /* Clear UART notify flag for the new link */
            ESP_LOGI(TAG, "UART notify state reset for new connection");

            int rc = ble_gattc_exchange_mtu(conn_handle, NULL, NULL);  /* Ask the phone for a larger MTU */
            if (rc != 0) {                                             /* Non-zero means the request was not queued */
                ESP_LOGW(TAG, "MTU exchange request failed; rc=%d", rc);
            } else {
                ESP_LOGI(TAG, "MTU exchange requested (%d bytes preferred)", PREFERRED_MTU);
            }
        } else {  /* The connection attempt failed; become discoverable again */
            ESP_LOGE(TAG, "Connect failed; status=%d — restarting advertising", event->connect.status);
            ble_app_advertise();  /* Broadcast the device name again so the phone can retry */
        }
        break;  /* Done handling CONNECT */

    case BLE_GAP_EVENT_DISCONNECT:  /* The phone dropped the link or walked out of range */
        ESP_LOGI(TAG, "GAP DISCONNECT: reason=%d", event->disconnect.reason);

        conn_handle = BLE_HS_CONN_HANDLE_NONE;  /* Mark the link as gone */
        current_mtu = 23;                       /* Restore default MTU for the next connection */
        ESP_LOGI(TAG, "Link cleared; handle=NONE, MTU=%u", current_mtu);

        ble_uart_on_link_reset();  /* Disable UART notifications until a new subscribe */
        ESP_LOGI(TAG, "UART notify state reset after disconnect");

        ble_app_advertise();  /* Start advertising immediately so another phone can connect */
        ESP_LOGI(TAG, "Advertising restarted after disconnect");
        break;  /* Done handling DISCONNECT */

    case BLE_GAP_EVENT_MTU:  /* Both devices finished negotiating packet size */
        current_mtu = event->mtu.value;  /* Store the agreed MTU */
        ESP_LOGI(TAG, "GAP MTU: updated to %u (payload up to %u bytes)",
                 current_mtu, ble_app_max_payload());
        break;  /* Done handling MTU */

    case BLE_GAP_EVENT_SUBSCRIBE:  /* Phone toggled notifications on a characteristic CCCD */
        ESP_LOGI(TAG, "GAP SUBSCRIBE: attr_handle=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        ble_uart_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify);  /* Let UART claim its TX CCCD */
        break;  /* Done handling SUBSCRIBE */

    case BLE_GAP_EVENT_ADV_COMPLETE:  /* Advertising duration ended (should be rare with FOREVER) */
        ESP_LOGI(TAG, "GAP ADV_COMPLETE: restarting advertising");
        ble_app_advertise();  /* Keep the device visible */
        break;  /* Done handling ADV_COMPLETE */

    default:  /* Ignore GAP events this demo does not use */
        ESP_LOGI(TAG, "GAP event %d ignored", event->type);
        break;
    }

    return 0;  /* Tell NimBLE the event was consumed successfully */
}

/* Build the advertising packet and start broadcasting the device name. */
static void ble_app_advertise(void)
{
    ESP_LOGI(TAG, "ble_app_advertise(): preparing advertisement packet");

    struct ble_hs_adv_fields fields = {0};             /* Zero the advert payload so unused fields stay clear */
    const char *name = ble_svc_gap_device_name();      /* Read the GAP device name set during init */
    ESP_LOGI(TAG, "Advertisement name from GAP: \"%s\"", name);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;  /* General discoverable, BLE only (no Classic) */
    fields.name = (uint8_t *)name;                                    /* Point the advert at the device-name bytes */
    fields.name_len = strlen(name);                                   /* Length of that name in bytes */
    fields.name_is_complete = 1;                                      /* Name fits fully in the advert packet */
    ESP_LOGI(TAG, "Advertisement flags set; name_len=%u", (unsigned)fields.name_len);

    int rc = ble_gap_adv_set_fields(&fields);  /* Load the payload into the controller */
    if (rc != 0) {                             /* Non-zero means the payload was rejected */
        ESP_LOGE(TAG, "adv fields failed; rc=%d", rc);
        return;                                /* Cannot start advertising without a valid payload */
    }
    ESP_LOGI(TAG, "Advertisement fields accepted by controller");

    struct ble_gap_adv_params adv = {0};       /* Zero connection/discoverable parameters */
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;     /* Undirected connectable: any scanner may connect */
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;     /* General discoverable: appears in normal phone scans */
    ESP_LOGI(TAG, "Advertisement params: connectable + general discoverable");

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv, ble_gap_event, NULL);  /* Broadcast forever; GAP events go to ble_gap_event */
    if (rc != 0) {                                      /* Non-zero means the radio did not start advertising */
        ESP_LOGE(TAG, "adv start failed; rc=%d", rc);
        return;                                         /* Leave without claiming we are visible */
    }

    ESP_LOGI(TAG, "Advertising as \"%s\"", name);  /* Confirm on the serial monitor that we are visible */
}

/* Called by NimBLE when the host stack has synchronized with the BLE radio hardware. */
static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "ble_on_sync(): host synchronized with radio");
    ble_att_set_preferred_mtu(PREFERRED_MTU);  /* Ask the stack to prefer 247-byte packets */
    ESP_LOGI(TAG, "Preferred MTU set to %d", PREFERRED_MTU);
    ble_app_advertise();                       /* Begin broadcasting so phones can find this board */
}

/* FreeRTOS task that runs the NimBLE event loop until the stack is shut down. */
static void ble_host_task(void *param)
{
    (void)param;  /* Unused task argument required by xTaskCreate / NimBLE port */
    ESP_LOGI(TAG, "ble_host_task(): NimBLE host loop starting (this task blocks here)");
    nimble_port_run();              /* Process BLE events forever; does not return during normal operation */
    ESP_LOGI(TAG, "ble_host_task(): nimble_port_run() returned, deinitializing");
    nimble_port_freertos_deinit();  /* Release host resources if the loop ever exits */
}

/* Initialize NVS-independent BLE pieces: stack, GAP/GATT, UART service, host task. */
void ble_app_init(void)
{
    ESP_LOGI(TAG, "ble_app_init(): starting NimBLE port");
    ESP_ERROR_CHECK(nimble_port_init());  /* Bring up the NimBLE host; abort if this fails */

    ble_hs_cfg.sync_cb = ble_on_sync;  /* Run ble_on_sync() as soon as the radio is ready (tells NimBLE BLE stack what function to run 
    once the BLE host and controller are fully initialized, synchronized, and ready to transmit/receive RF signals.)*/
    ESP_LOGI(TAG, "Host sync callback registered (ble_on_sync)");

    ble_att_set_preferred_mtu(PREFERRED_MTU);  /* Configure preferred MTU before the host task starts */
    ESP_LOGI(TAG, "Preferred MTU configured to %d before host start", PREFERRED_MTU);

    ble_svc_gap_init();                         /* Enable the standard GAP service */
    ESP_LOGI(TAG, "Standard GAP service initialized");
    ble_svc_gatt_init();                        /* Enable the standard GATT service */
    ESP_LOGI(TAG, "Standard GATT service initialized");
    ble_svc_gap_device_name_set(DEVICE_NAME);   /* Set the over-the-air name to ESP32_BLE */
    ESP_LOGI(TAG, "GAP device name set to \"%s\"", DEVICE_NAME);

   
    ESP_LOGI(TAG, "Counting GATT resources for Nordic UART service");
    int rc = ble_uart_count_cfg();  /* Tell NimBLE how many attributes UART will need */
    ESP_LOGI(TAG, "ble_uart_count_cfg() returned %d", rc);
    assert(rc == 0);                /* Halt in debug builds if resource counting failed */

    ESP_LOGI(TAG, "Adding Nordic UART service to the GATT database");
    rc = ble_uart_add_svcs();       /* Install RX/TX characteristics into the attribute table */
    ESP_LOGI(TAG, "ble_uart_add_svcs() returned %d", rc);
    assert(rc == 0);                /* Halt in debug builds if service registration failed */

    ESP_LOGI(TAG, "Launching NimBLE host FreeRTOS task");
    nimble_port_freertos_init(ble_host_task);  /* Create the background task that runs nimble_port_run() */
    ESP_LOGI(TAG, "ble_app_init(): BLE core ready");
}
