#pragma once  /* Include this header at most once per translation unit */

#include <stdbool.h>  /* Provides the bool type used by subscribe / notify flags */
#include <stdint.h>   /* Provides uint16_t for GATT attribute handles */

/*
 * Nordic UART Service (NUS): RX writes from the phone, TX notifications to the phone.
 * Count GATT resources before adding services so extra profiles can be registered later.
 */

int ble_uart_count_cfg(void);                                          /* Count GATT records this UART service needs */
int ble_uart_add_svcs(void);                                           /* Register the UART service table with NimBLE */
void ble_uart_send(const char *msg);                                   /* Notify the connected phone with a text string */
void ble_uart_on_link_reset(void);                                     /* Clear notify state on connect / disconnect */
void ble_uart_on_subscribe(uint16_t attr_handle, bool notify);         /* Update notify flag when the phone toggles CCCD */
