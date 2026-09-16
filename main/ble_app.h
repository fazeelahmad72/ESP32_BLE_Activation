#pragma once  /* Include this header at most once per translation unit */

#include <stdbool.h>  /* Provides the bool type used by ble_app_is_connected() */
#include <stdint.h>   /* Provides uint16_t for connection handle and MTU values */

/*
 * BLE application core: NimBLE bring-up, advertising, and connection state.
 * GATT feature modules (UART today, others later) register from ble_app_init().
 */

void ble_app_init(void);                 /* Initialize NimBLE, register GATT services, start the host task */
uint16_t ble_app_conn_handle(void);      /* Return the active connection handle, or NONE if disconnected */
bool ble_app_is_connected(void);         /* Return true when a phone currently holds a BLE link */
uint16_t ble_app_mtu(void);              /* Return the negotiated MTU size in bytes */
uint16_t ble_app_max_payload(void);      /* Return max application bytes per packet (MTU minus ATT header) */
