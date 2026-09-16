idf_component_register(SRCS "main.c" "ble_app.c" "ble_uart.c" "console.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash)
