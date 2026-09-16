/* Standard C libraries */
#include <string.h>  /* Provides strlen() for measuring the string to send */

/* ESP-IDF system headers */
#include "esp_log.h"            /* Logging framework for formatted terminal output (ESP_LOGI, etc.) */
#include "freertos/FreeRTOS.h"  /* Core FreeRTOS types used by vTaskDelay() */
#include "freertos/task.h"      /* vTaskDelay() to pause between notification chunks */

/* NimBLE protocol-layer headers */
#include "host/ble_hs.h"    /* Host helpers: mbufs, connection handles, GATT notify */
#include "host/ble_uuid.h"  /* BLE_UUID128_INIT for 128-bit Nordic UART UUIDs */
#include "host/ble_gatt.h"  /* GATT access context, service/characteristic tables */
#include "host/ble_att.h"   /* BLE_ATT_ERR_UNLIKELY for unhandled access opcodes */

/* Project modules */
#include "ble_config.h"  /* BLE_LINE_MAX for receive buffer size */
#include "ble_app.h"     /* Connection / MTU getters used while sending */
#include "ble_uart.h"    /* Public UART API implemented in this file */

static const char *TAG = "BLE_UART";  /* Log tag so monitor output can be filtered to this module */

static uint16_t tx_val_handle;     /* Stack-assigned handle of the TX characteristic (needed for notify) */
static bool notify_enabled = false; /* True only after the phone enables notifications on TX */

/* Nordic UART Service UUID (128-bit), matching Nordic Semiconductor's NUS specification */
static const ble_uuid128_t uart_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

/* RX characteristic UUID: phone WRITES data to the ESP32 on this attribute */
static const ble_uuid128_t uart_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

/* TX characteristic UUID: ESP32 NOTIFIES data to the phone on this attribute */
static const ble_uuid128_t uart_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/*
 * GATT access callback.
 * NimBLE calls this when the phone reads or writes a UART characteristic.
 */
static int uart_chr_access(uint16_t conn_handle_in, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle_in;  /* Connection handle is not needed for this demo's RX print path */
    (void)attr_handle;     /* Attribute handle is not needed because RX/TX share this callback */
    (void)arg;             /* User argument is unused */

    ESP_LOGI(TAG, "uart_chr_access(): opcode=%d", ctxt->op);  /* Print whether this is a read or a write */

    switch (ctxt->op) {  /* Branch on the GATT operation the phone performed */
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {  /* Phone wrote bytes into the RX characteristic */
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);  /* Count how many bytes arrived in the mbuf chain */
        char buf[BLE_LINE_MAX + 1];               /* Local C string to hold the unpacked text */
        ESP_LOGI(TAG, "RX write received: %u raw bytes", len);

        if (len > BLE_LINE_MAX) {  /* Cap the copy so buf cannot overflow */
            ESP_LOGW(TAG, "RX length %u exceeds BLE_LINE_MAX %d — truncating", len, BLE_LINE_MAX);
            len = BLE_LINE_MAX;    /* Keep only as many bytes as the buffer can store */
        }

        ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);  /* Copy mbuf bytes into a contiguous array */
        buf[len] = '\0';                                /* Add a terminator so %s can print the text */
        ESP_LOGI(TAG, "From phone (%u bytes): %s", len, buf);  /* Show the message on the USB serial monitor */
        return 0;                                       /* 0 tells the stack the write was accepted */
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:  /* Phone tried to read a characteristic value */
        ESP_LOGI(TAG, "RX/TX read requested — no stored value, returning success with empty body");
        return 0;  /* Success with no payload is enough for this UART demo */

    default:  /* Any other access type is not supported */
        ESP_LOGW(TAG, "Unhandled GATT access opcode %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;  /* Report an unlikely/unsupported ATT error */
    }
}

/* Send a C string to the phone as one or more GATT notifications on TX. */
void ble_uart_send(const char *msg)
{
    ESP_LOGI(TAG, "ble_uart_send(): called with \"%s\"", msg);

    if (!ble_app_is_connected() || !notify_enabled) {  /* Need both a live link and TX notify enabled */
        ESP_LOGW(TAG, "Not connected or Notify not enabled in nRF Connect (connected=%d notify=%d)",
                 (int)ble_app_is_connected(), (int)notify_enabled);
        return;  /* Drop the message instead of calling notify on an invalid link */
    }

    size_t total = strlen(msg);                 /* Number of bytes in the caller's string */
    size_t max_payload = ble_app_max_payload(); /* Max bytes the current MTU can carry */
    size_t offset = 0;                          /* Index of the next byte still unsent */
    ESP_LOGI(TAG, "Sending %u bytes (MTU=%u, max payload=%u)",
             (unsigned)total, ble_app_mtu(), (unsigned)max_payload);

    while (offset < total) {  /* Repeat until every byte has been queued as a notification */
        size_t chunk = total - offset;  /* Remaining bytes from the current offset */
        if (chunk > max_payload) {      /* This remainder does not fit in one packet */
            chunk = max_payload;        /* Send only what the MTU allows */
        }
        ESP_LOGI(TAG, "Notify chunk: offset=%u size=%u", (unsigned)offset, (unsigned)chunk);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg + offset, chunk);  /* Copy this slice into an mbuf */
        if (om == NULL) {                                                 /* Heap/mbuf pool exhausted */
            ESP_LOGE(TAG, "mbuf alloc failed at offset %u", (unsigned)offset);
            return;                                                       /* Stop sending; later bytes are dropped */
        }
        ESP_LOGI(TAG, "mbuf allocated for %u bytes", (unsigned)chunk);

        int rc = ble_gatts_notify_custom(ble_app_conn_handle(), tx_val_handle, om);  /* Push the packet to the phone */
        if (rc != 0) {                                                               /* Stack rejected the notify */
            ESP_LOGE(TAG, "notify failed; rc=%d at offset %u", rc, (unsigned)offset);
            return;                                                                  /* Abort the rest of the message */
        }
        ESP_LOGI(TAG, "notify queued; rc=%d", rc);

        offset += chunk;  /* Advance past the bytes just sent */

        if (offset < total) {                         /* More chunks remain */
            ESP_LOGI(TAG, "Waiting 20 ms before next chunk to avoid radio congestion");
            vTaskDelay(pdMS_TO_TICKS(20));            /* Brief pause between packets */
        }
    }

    ESP_LOGI(TAG, "To phone: %s", msg);  /* Confirm the full string was queued */
}

/* Called from GAP connect/disconnect so UART does not keep a stale notify flag. */
void ble_uart_on_link_reset(void)
{
    notify_enabled = false;  /* Phone must enable Notify again after the next connection */
    ESP_LOGI(TAG, "ble_uart_on_link_reset(): notify_enabled=false");
}

/* Called from GAP SUBSCRIBE; accept the event only if it targets our TX characteristic. */
void ble_uart_on_subscribe(uint16_t attr_handle, bool notify)
{
    ESP_LOGI(TAG, "ble_uart_on_subscribe(): attr_handle=%u tx_val_handle=%u notify=%d",
             attr_handle, tx_val_handle, (int)notify);

    if (attr_handle == tx_val_handle) {  /* This subscribe belongs to UART TX, not some other characteristic */
        notify_enabled = notify;         /* Store whether the phone now wants notifications */
        ESP_LOGI(TAG, "Notify %s", notify_enabled ? "ENABLED" : "DISABLED");
    } else {
        ESP_LOGI(TAG, "Subscribe was not for UART TX — ignored");
    }
}

/* Characteristic table: RX (write) then TX (notify). The {0} entry terminates the list. */
static const struct ble_gatt_chr_def uart_chrs[] = {
    {
        .uuid = &uart_rx_uuid.u,                                          /* RX UUID: phone -> ESP32 */
        .access_cb = uart_chr_access,                                     /* Writes land in uart_chr_access() */
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,      /* Allow Write and Write Without Response */
    },
    {
        .uuid = &uart_tx_uuid.u,           /* TX UUID: ESP32 -> phone */
        .access_cb = uart_chr_access,      /* Reads (if any) use the same callback */
        .flags = BLE_GATT_CHR_F_NOTIFY,    /* Phone may enable notifications on this characteristic */
        .val_handle = &tx_val_handle,      /* NimBLE writes the assigned handle back here */
    },
    { 0 },  /* End of characteristic list */
};

/* Service table: one primary Nordic UART service. The {0} entry terminates the list. */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,  /* Primary service visible during discovery */
        .uuid = &uart_svc_uuid.u,           /* Nordic UART Service UUID */
        .characteristics = uart_chrs,       /* Attach the RX/TX characteristic list */
    },
    { 0 },  /* End of service list */
};

int ble_uart_count_cfg(void)
{
    ESP_LOGI(TAG, "ble_uart_count_cfg(): counting UART GATT resources");
    int rc = ble_gatts_count_cfg(gatt_svcs);  /* Accumulate attribute / CCCD counts in the host */
    ESP_LOGI(TAG, "ble_uart_count_cfg(): rc=%d", rc);
    return rc;  /* 0 means the host accepted the resource count */
}

int ble_uart_add_svcs(void)
{
    ESP_LOGI(TAG, "ble_uart_add_svcs(): registering UART service (RX write, TX notify)");
    int rc = ble_gatts_add_svcs(gatt_svcs);  /* Insert the service into the GATT database */
    ESP_LOGI(TAG, "ble_uart_add_svcs(): rc=%d tx_val_handle=%u", rc, tx_val_handle);
    return rc;  /* 0 means RX/TX are now discoverable by the phone */
}
