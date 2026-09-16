/* Standard C Input/Output library */
#include <stdio.h>  /* Provides printf(), getchar(), putchar(), setvbuf() */

/* ESP-IDF system headers */
#include "esp_log.h"            /* Logging framework for formatted terminal output (ESP_LOGI, etc.) */
#include "freertos/FreeRTOS.h"  /* Core FreeRTOS operating system structures */
#include "freertos/task.h"      /* xTaskCreate, vTaskDelay for the console listener task */

/* Project modules */
#include "ble_config.h"  /* BLE_LINE_MAX: maximum characters collected before Enter */
#include "ble_uart.h"    /* ble_uart_send() forwards a completed line to the phone */
#include "console.h"     /* Public console_start() implemented in this file */

static const char *TAG = "CONSOLE";  /* Log tag so monitor output can be filtered to this module */

/*
 * FreeRTOS background task: USB-serial listener.
 * Collects typed characters until Enter, then sends the line over BLE UART.
 */
static void console_to_ble_task(void *arg)
{
    (void)arg;  /* Unused task argument required by xTaskCreate */

    char line[BLE_LINE_MAX + 1];  /* Buffer that accumulates one typed line */
    size_t pos = 0;               /* Index of the next free slot in line[] */

    ESP_LOGI(TAG, "console_to_ble_task(): started");

    setvbuf(stdin, NULL, _IONBF, 0);   /* Disable stdin buffering so keys are read immediately */
    ESP_LOGI(TAG, "stdin set to unbuffered mode");
    setvbuf(stdout, NULL, _IONBF, 0);  /* Disable stdout buffering so echo appears immediately */
    ESP_LOGI(TAG, "stdout set to unbuffered mode");

    ESP_LOGI(TAG, "Type text (up to %d chars), then press Enter", BLE_LINE_MAX);

    while (1) {  /* Run forever; this task is the console's main loop */
        int c = getchar();  /* Read one character from the USB serial monitor */

        if (c == EOF) {                         /* No character available right now */
            vTaskDelay(pdMS_TO_TICKS(10));      /* Yield so other tasks (BLE host) can run */
            continue;                           /* Try getchar() again */
        }

        ESP_LOGI(TAG, "Key received: code=%d char='%c'", c, (c >= 32 && c <= 126) ? (char)c : '?');

        if (c == '\n' || c == '\r') {  /* Enter (LF or CR) means the line is complete */
            printf("\r\n");            /* Move the local cursor to a new line */
            fflush(stdout);            /* Force the newline to appear on the monitor */
            ESP_LOGI(TAG, "Enter pressed; collected %u characters", (unsigned)pos);

            if (pos == 0) {                         /* User pressed Enter on an empty line */
                ESP_LOGI(TAG, "Empty line ignored");
                continue;                           /* Do not send an empty notification */
            }

            line[pos] = '\0';                       /* Terminate the collected characters as a C string */
            ESP_LOGI(TAG, "Sending line to BLE UART: \"%s\"", line);
            ble_uart_send(line);                    /* Notify the phone with this line */
            pos = 0;                                /* Reset so the next line starts empty */
            ESP_LOGI(TAG, "Line buffer reset; ready for next input");
            continue;                               /* Wait for the next key */
        }

        if (c == 0x08 || c == 0x7f) {  /* Backspace or DEL */
            ESP_LOGI(TAG, "Backspace received; pos=%u", (unsigned)pos);
            if (pos > 0) {             /* Only erase if there is a character to remove */
                pos--;                 /* Drop the last stored character */
                printf("\b \b");       /* Erase it visually on the monitor: back, space, back */
                fflush(stdout);        /* Show the erase immediately */
                ESP_LOGI(TAG, "Erased last character; pos now %u", (unsigned)pos);
            } else {
                ESP_LOGI(TAG, "Backspace ignored; buffer already empty");
            }
            continue;  /* Wait for the next key */
        }

        if (c < 32 || c > 126) {  /* Non-printable control character */
            ESP_LOGI(TAG, "Control character %d ignored", c);
            continue;             /* Do not store it in the line buffer */
        }

        if (pos < BLE_LINE_MAX) {          /* Room remains in the line buffer */
            line[pos++] = (char)c;         /* Append the printable character and advance pos */
            putchar(c);                    /* Echo the character back to the USB monitor */
            fflush(stdout);                /* Show the echo immediately */
            ESP_LOGI(TAG, "Stored '%c'; pos now %u", (char)c, (unsigned)pos);
        } else {
            ESP_LOGW(TAG, "Line full (%d chars); ignoring extra character '%c'", BLE_LINE_MAX, (char)c);
        }
    }
}

/* Create the console FreeRTOS task. Called once from app_main(). */
void console_start(void)
{
    ESP_LOGI(TAG, "console_start(): creating task \"console_ble\" (stack=4096, priority=5)");
    BaseType_t ok = xTaskCreate(console_to_ble_task, "console_ble", 4096, NULL, 5, NULL);  /* Spawn the listener */
    if (ok == pdPASS) {  /* Task was created and is ready to run */
        ESP_LOGI(TAG, "console_start(): task created successfully");
    } else {             /* Not enough heap for the stack/TCB */
        ESP_LOGE(TAG, "console_start(): xTaskCreate failed");
    }
}
