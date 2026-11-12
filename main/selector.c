/*
 * Selector host driver (basic): sends the IP / MAC pages to the 16x2 LCD.
 * Only the 'S' (screen) function is used. Replies and keys are not handled yet;
 * received bytes are discarded.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "selector.h"
#include "wifi_manager.h"
#include "ethernet_manager.h"

#define TAG "SELECTOR"

#define SELECTOR_UART UART_NUM_2
#define SELECTOR_TX_PIN 4 //TODO: CHANGE FOR THE NEW PROTOTYPE!
#define SELECTOR_RX_PIN 36 //TODO: CHANGE FOR THE NEW PROTOTYPE!
#define SELECTOR_BAUD 9600

#define SEL_START 0x7E
#define SEL_STOP 0x81
#define SEL_ID 0x01
#define SEL_FUNC_SCREEN 'S'
#define SEL_LCD_COLS 16
#define SEL_SCREEN_LEN 38 // 4 header + 32 text + checksum + stop

#define REFRESH_MS 200  // screen refresh period
#define PAGE_MS 3000    // time each page is shown
#define MAX_PAGES 4

uint8_t selector_checksum(const uint8_t *frame, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i + 2 < len; i++)
    {
        x ^= frame[i];
    }
    return x ^ 0xFF;
}

size_t selector_build_screen(uint8_t *out, const char *line1, const char *line2)
{
    out[0] = SEL_START;
    out[1] = SEL_SCREEN_LEN;
    out[2] = SEL_ID;
    out[3] = SEL_FUNC_SCREEN;
    memset(&out[4], ' ', 2 * SEL_LCD_COLS);

    size_t n1 = strnlen(line1, SEL_LCD_COLS);
    size_t n2 = strnlen(line2, SEL_LCD_COLS);
    memcpy(&out[4], line1, n1);
    memcpy(&out[4 + SEL_LCD_COLS], line2, n2);

    out[SEL_SCREEN_LEN - 2] = selector_checksum(out, SEL_SCREEN_LEN);
    out[SEL_SCREEN_LEN - 1] = SEL_STOP;
    return SEL_SCREEN_LEN;
}

typedef struct
{
    char l1[SEL_LCD_COLS + 1];
    char l2[SEL_LCD_COLS + 1];
} page_t;

// Adds IP and MAC pages for one interface when it has an IP.
static int add_iface_pages(page_t *pages, int count, const char *name, esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    uint8_t mac[6];

    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
    {
        return count;
    }

    if (count < MAX_PAGES)
    {
        snprintf(pages[count].l1, sizeof(pages[count].l1), "%s IP", name);
        snprintf(pages[count].l2, sizeof(pages[count].l2), IPSTR, IP2STR(&info.ip));
        count++;
    }
    if (count < MAX_PAGES && esp_netif_get_mac(netif, mac) == ESP_OK)
    {
        // 17 chars with colons do not fit in 16 columns: show 12 hex digits
        snprintf(pages[count].l1, sizeof(pages[count].l1), "%s MAC", name);
        snprintf(pages[count].l2, sizeof(pages[count].l2), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        count++;
    }
    return count;
}

static void selector_task(void *arg)
{
    page_t pages[MAX_PAGES];
    uint8_t frame[SEL_SCREEN_LEN];
    uint8_t rx[32];
    int page = 0;
    TickType_t page_since = xTaskGetTickCount();

    while (1)
    {
        int count = 0;
        count = add_iface_pages(pages, count, "WIFI", wifi_get_netif());
        count = add_iface_pages(pages, count, "ETH", ethernet_get_netif());

        if (count == 0)
        {
            snprintf(pages[0].l1, sizeof(pages[0].l1), "MODULO WIFI");
            snprintf(pages[0].l2, sizeof(pages[0].l2), "SEM REDE");
            count = 1;
        }

        if (xTaskGetTickCount() - page_since >= pdMS_TO_TICKS(PAGE_MS))
        {
            page++;
            page_since = xTaskGetTickCount();
        }
        if (page >= count)
        {
            page = 0;
        }

        size_t len = selector_build_screen(frame, pages[page].l1, pages[page].l2);
        uart_write_bytes(SELECTOR_UART, (const char *)frame, len);

        // Replies (ACK + key state) are not used yet: discard them
        uart_read_bytes(SELECTOR_UART, rx, sizeof(rx), 0);

        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

void selector_start(void)
{
    // Self-test against the known-good vector from the alarm firmware
    static const uint8_t vec[8] = {0x7E, 0x08, 0x01, 0x3F, 0xB4, 0xB4, 0xB7, 0x81};
    if (selector_checksum(vec, sizeof(vec)) != 0xB7)
    {
        ESP_LOGE(TAG, "Checksum self-test failed");
        return;
    }

    const uart_config_t cfg = {
        .baud_rate = SELECTOR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(SELECTOR_UART, 256, 256, 0, NULL, 0);
    if (err == ESP_OK)
    {
        err = uart_param_config(SELECTOR_UART, &cfg);
    }
    if (err == ESP_OK)
    {
        err = uart_set_pin(SELECTOR_UART, SELECTOR_TX_PIN, SELECTOR_RX_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return;
    }

    if (xTaskCreate(selector_task, "selector", 3072, NULL, 4, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Task create failed");
        return;
    }
    ESP_LOGI(TAG, "Started on UART%d (TX GPIO%d, RX GPIO%d)", SELECTOR_UART, SELECTOR_TX_PIN, SELECTOR_RX_PIN);
}
