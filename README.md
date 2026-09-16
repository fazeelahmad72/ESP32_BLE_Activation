# ESP32 BLE Activation

ESP-IDF firmware that turns a classic **ESP32** into a BLE peripheral with a **Nordic UART Service (NUS)** link to a phone.

After boot the board advertises as `ESP32_BLE`. A phone app such as nRF Connect can connect, send text to the USB serial monitor, and receive lines you type in the monitor.

## Features

- NimBLE host (not Bluedroid)
- Nordic UART Service: phone writes RX, ESP32 notifies TX
- USB-serial console forwards typed lines over BLE
- Preferred MTU of 247 bytes (payload up to 244 bytes after the ATT header)
- Bonds stored in NVS (up to 3 phones)
- 4 MB flash with a two-OTA partition table

## Hardware

- ESP32 DevKit (original ESP32, not C3/S3)
- 4 MB flash (required; the OTA layout does not fit on 2 MB)

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) with `IDF_PATH` set
- USB cable and a serial terminal (ESP-IDF Monitor, PuTTY, or similar)
- Phone app: [nRF Connect](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile) (Android / iOS)

## Build and flash

From the project root, with ESP-IDF exported:

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

On Linux or macOS, replace `COMx` with `/dev/ttyUSB0` or `/dev/cu.usbserial-*`.

On first boot you should see advertising logs for `ESP32_BLE`.

## Try it with a phone

1. Open nRF Connect and scan for **ESP32_BLE**.
2. Connect, then open the Nordic UART service.
3. Enable notifications on the **TX** characteristic (otherwise the phone will not show text from the board).
4. Write ASCII text to the **RX** characteristic — it appears on the serial monitor as `From phone ...`.
5. Type a line in the serial monitor and press Enter — it is sent to the phone as a TX notification.

Empty lines are ignored. Backspace works in the console. Disconnecting restarts advertising automatically.

## BLE identity

| Item | Value |
|------|--------|
| Advertised name | `ESP32_BLE` |
| NUS service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (phone writes) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (ESP32 notifies) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Shared tunables (device name, MTU) live in `main/ble_config.h`.

## Project layout

```
esp32_ble_activation/
├── CMakeLists.txt          Project name and ESP-IDF boilerplate
├── sdkconfig.defaults      Chip, NimBLE, flash, and OTA defaults
├── main/
│   ├── main.c              Boot: NVS, BLE, console task
│   ├── ble_app.c/.h        NimBLE bring-up, advertising, GAP events
│   ├── ble_uart.c/.h       Nordic UART GATT service
│   ├── console.c/.h        USB-serial → BLE forwarder
│   └── ble_config.h        Device name and packet-size limits
```

## Configuration notes

`sdkconfig.defaults` is the shareable config. A generated local `sdkconfig` is gitignored.

Important defaults:

- Target: `esp32`
- Flash size: 4 MB
- Partition table: two OTA slots (`ota_0` + `ota_1` + `otadata`)
- NimBLE peripheral + GATT server (NUS)
- GATT client enabled only so MTU exchange (`ble_gattc_exchange_mtu`) can run

If your board is not 4 MB flash, change `CONFIG_ESPTOOLPY_FLASHSIZE` before building.

## Runtime flow

1. Power-on: `app_main()` initializes NVS, starts NimBLE, and launches the USB-serial console task.
2. Radio ready: the board advertises as `ESP32_BLE`.
3. Phone connects: the link handle is saved and an MTU exchange is requested (preferred 247 bytes).
4. Phone enables TX notifications: the board can send text to the phone.
5. Phone writes RX: text is printed on the USB serial monitor.
6. You type in the serial monitor and press Enter: the line is sent to the phone as a TX notification.
7. Disconnect: connection state is cleared and advertising starts again.
