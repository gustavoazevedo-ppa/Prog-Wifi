#include "udp_broadcast.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

#define TAG "UDPBCAST"
#define BROADCAST_INTERVAL_MS 5000
#define BROADCAST_TASK_STACK  2048
#define BROADCAST_TASK_PRIO   4

static uint8_t s_payload[128];
static size_t s_payload_len = 0;
static uint16_t s_port = 0;
static bool s_task_started = false;

static void send_on_interface(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK)
        return;

    // Skip interfaces with no IP
    if (ip_info.ip.addr == 0)
        return;

    // Compute broadcast address: ip | ~netmask
    uint32_t broadcast_addr = ip_info.ip.addr | ~ip_info.netmask.addr;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Socket creation failed: %d", errno);
        return;
    }

    // Enable broadcast on this socket
    int broadcast_en = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_en, sizeof(broadcast_en));

    // Bind to the specific interface IP so the packet goes out the right NIC
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0), // any source port
        .sin_addr.s_addr = ip_info.ip.addr,
    };
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    // Send to the interface's broadcast address
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(s_port),
        .sin_addr.s_addr = broadcast_addr,
    };

    int ret = sendto(sock, s_payload, s_payload_len, 0,
                     (struct sockaddr *)&dest, sizeof(dest));
    if (ret < 0)
    {
        ESP_LOGW(TAG, "sendto failed on " IPSTR ": %d", IP2STR(&ip_info.ip), errno);
    }
    else
    {
        ESP_LOGD(TAG, "Broadcast %d bytes to " IPSTR " via " IPSTR,
                 ret, IP2STR((esp_ip4_addr_t *)&broadcast_addr), IP2STR(&ip_info.ip));
    }

    closesocket(sock);
}

static void udp_broadcast_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));

        // Iterate all registered network interfaces
        esp_netif_t *netif = NULL;
        while ((netif = esp_netif_next_unsafe(netif)) != NULL)
        {
            if (esp_netif_is_netif_up(netif))
            {
                send_on_interface(netif);
            }
        }
    }
}

void udp_broadcast_start(const uint8_t *payload, size_t len, uint16_t port)
{
    if (len > sizeof(s_payload))
    {
        ESP_LOGE(TAG, "Payload too large (%d > %d)", (int)len, (int)sizeof(s_payload));
        return;
    }

    memcpy(s_payload, payload, len);
    s_payload_len = len;
    s_port = port;

    if (!s_task_started)
    {
        xTaskCreate(udp_broadcast_task, "udp_bcast", BROADCAST_TASK_STACK, NULL, BROADCAST_TASK_PRIO, NULL);
        s_task_started = true;
        ESP_LOGI(TAG, "UDP broadcast task started (port %u, %u bytes, every %d ms)",
                 port, (unsigned)len, BROADCAST_INTERVAL_MS);
    }
    else
    {
        ESP_LOGI(TAG, "UDP broadcast payload updated (%u bytes)", (unsigned)len);
    }
}

void udp_broadcast_set_payload(const uint8_t *payload, size_t len)
{
    if (len > sizeof(s_payload))
    {
        ESP_LOGE(TAG, "Payload too large (%d > %d)", (int)len, (int)sizeof(s_payload));
        return;
    }

    memcpy(s_payload, payload, len);
    s_payload_len = len;
    ESP_LOGI(TAG, "UDP broadcast payload updated (%u bytes)", (unsigned)len);
}
