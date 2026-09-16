# ESP32 BLE Activation Workflow

Here is the complete step-by-step execution flow of your program, from the moment power is applied to the ESP32 to the moment you exchange data with a smartphone.

## Step 1: System Boot & Entry Point (`app_main`)

When the ESP32 powers on, the processor jumps directly into `app_main(void)`.

### Initialize NVS Storage

`nvs_flash_init()` runs first. The Bluetooth stack requires flash memory access to read board configurations and store security keys.

### Initialize NimBLE Port

`nimble_port_init()` starts the core Bluetooth stack software engine.

### Register Stack Sync Callback

`ble_hs_cfg.sync_cb = ble_on_sync;` tells NimBLE: "As soon as the Bluetooth radio hardware finishes turning on, execute the `ble_on_sync()` function."

### Initialize Standard Services & Set Device Name

`ble_svc_gap_init()` and `ble_svc_gatt_init()` set up standard Bluetooth services. `ble_svc_gap_device_name_set("ESP32_BLE")` assigns your board's advertised name.

### Register Custom GATT Services

`ble_gatts_add_svcs(gatt_svcs)` loads your Nordic UART service table (containing the RX and TX characteristics) into the Bluetooth database.

### Spawn Background FreeRTOS Tasks

`nimble_port_freertos_init(ble_host_task);` launches the NimBLE host engine in its own background task.

`xTaskCreate(console_to_ble_task, ...);` launches your custom terminal listener task in a second background task.

---

## Step 2: Radio Synchronization & Advertising (`ble_on_sync`)

Once the NimBLE host stack successfully connects to the physical Bluetooth radio hardware, it automatically triggers `ble_on_sync()`.

### Set Preferred MTU

`ble_att_set_preferred_mtu(247)` sets the desired maximum packet payload size.

### Trigger Advertising (`ble_app_advertise`)

- Builds the advertising packet payload containing your device name (`ESP32_BLE`) and flags (`fields.flags = ...`).
- Starts broadcasting over the air using `ble_gap_adv_start()`.
- Passes `ble_gap_event` as the event callback function.

### Idle State

The ESP32 is now wirelessly shouting its presence to any phone nearby scanning via apps like nRF Connect.

---

## Step 3: Event 1 — Phone Connects (`ble_gap_event`)

When a user opens their phone app and taps Connect:

### Interrupt Trigger

The radio catches the incoming connection request and calls `ble_gap_event()` with the event type `BLE_GAP_EVENT_CONNECT`.

### Save Handle

The ESP32 saves the phone's unique connection identifier into `conn_handle`.

### Negotiate MTU

`ble_gattc_exchange_mtu(...)` sends a request over the air asking the phone: "Can we send up to 247 bytes per packet instead of the default 23 bytes?"

### Handle Update

When the phone responds, `ble_gap_event()` catches `BLE_GAP_EVENT_MTU` and updates `current_mtu`.

### Enable Notifications

When the phone enables notifications on the TX characteristic, `ble_gap_event()` catches `BLE_GAP_EVENT_SUBSCRIBE` and sets `notify_enabled = true`.

---

## Step 4: Data Flow A — Computer Terminal to Phone

```
[ Your Typing ] ---> console_to_ble_task() ---> ble_send_to_phone() ---> GATT Notify ---> [ Phone Screen ]
```

### Listen for Input

The background task `console_to_ble_task()` sits inside a `while(1)` loop, calling `getchar()` to read keyboard input line-by-line.

### Detect Enter Key

When you press Enter (`\n` or `\r`), it null-terminates the character array (`line`) and calls `ble_send_to_phone(line)`.

### Chunking & Transmit

`ble_send_to_phone()` checks that `conn_handle` is valid and `notify_enabled` is true.

It splits the message into chunks matching the negotiated MTU limit (`ble_max_payload()`).

It allocates an `os_mbuf` memory buffer and calls `ble_gatts_notify_custom()`.

### Air Transfer

The radio transmits the packet, and your text appears on the phone screen.

---

## Step 5: Data Flow B — Phone to Computer Terminal

```
[ Phone Sends Text ] ---> Bluetooth Radio ---> uart_chr_access() ---> ESP_LOGI ---> [ Computer Terminal ]
```

### Phone Writes Data

The phone app writes text to the RX Characteristic (`uart_rx_uuid`).

### Callback Trigger

NimBLE receives the wireless payload and automatically jumps into `uart_chr_access()`.

### Extract Payload

The `switch (ctxt->op)` statement matches `BLE_GATT_ACCESS_OP_WRITE_CHR`.

### Flatten & Print

`ble_hs_mbuf_to_flat()` copies raw packet bytes from the internal buffer into a local array (`buf`), adds a null terminator (`'\0'`), and prints it to your computer screen using `ESP_LOGI(TAG, "From phone ...", buf)`.

---

## Step 6: Event 2 — Phone Disconnects (`ble_gap_event`)

When the phone disconnects or moves out of range:

### Disconnect Event

`ble_gap_event()` catches `BLE_GAP_EVENT_DISCONNECT`.

### Reset State

`conn_handle` resets to `BLE_HS_CONN_HANDLE_NONE`, `notify_enabled` sets to `false`, and `current_mtu` resets back to `23`.

### Restart Advertising

It immediately calls `ble_app_advertise()` so the ESP32 becomes visible for another device to connect.
