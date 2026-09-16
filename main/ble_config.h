#pragma once  /* Include this header at most once per translation unit */

/*
 * Shared application tunables.
 * Keep device identity and packet-size limits here so new modules
 * (sensors, GPIO, extra BLE services) can reuse the same values.
 */

#define DEVICE_NAME      "ESP32_BLE"               /* Wireless name advertised over the air to mobile apps */
#define PREFERRED_MTU    247                       /* Desired Maximum Transmission Unit size in bytes */
#define BLE_LINE_MAX     (PREFERRED_MTU - 3)       /* Maximum raw text length (MTU minus 3-byte ATT header) */
