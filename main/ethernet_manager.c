#include "ethernet_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_enc28j60.h"
#include "driver/spi_master.h"
#include "nvs_flash.h"
#include "nvs.h"

#define ETHERNET_ENABLED 0 // 1 = Ethernet on, 0 = Ethernet disabled (driver never started, Wi-Fi only)

#define CONFIG_EXAMPLE_ENC28J60_SPI_HOST 2
#define CONFIG_EXAMPLE_ENC28J60_SCLK_GPIO 18
#define CONFIG_EXAMPLE_ENC28J60_MOSI_GPIO 23
#define CONFIG_EXAMPLE_ENC28J60_MISO_GPIO 19
#define CONFIG_EXAMPLE_ENC28J60_CS_GPIO 5
#define CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ 20
#define CONFIG_EXAMPLE_ENC28J60_INT_GPIO 4

bool ethernet_connected = false;
EventGroupHandle_t eth_event_group;
#define ETH_CONNECTED_BIT BIT0

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_mac_t *s_eth_mac = NULL;

#define TAG "ETHMGR"
#define ETH_NVS_NAMESPACE "eth"

// Forward declarations for static helpers
static void ethernet_apply_saved_config(void);
static bool ethernet_apply_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2);

// Modified Ethernet event handler
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "eth_event_handler: event_base=%s, event_id=%ld", event_base, event_id);
    if (event_base == ETH_EVENT)
    {
        switch (event_id)
        {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            ethernet_connected = true;
            xEventGroupSetBits(eth_event_group, ETH_CONNECTED_BIT);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            ethernet_connected = false;
            xEventGroupClearBits(eth_event_group, ETH_CONNECTED_BIT);
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// Ethernet initialization function
bool ethernet_is_enabled(void)
{
    return ETHERNET_ENABLED != 0;
}

void ethernet_init(void)
{
    if (!ETHERNET_ENABLED)
    {
        ESP_LOGW(TAG, "Ethernet disabled (ETHERNET_ENABLED=0): driver not started");
        return;
    }

    eth_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_EXAMPLE_ENC28J60_MISO_GPIO,
        .mosi_io_num = CONFIG_EXAMPLE_ENC28J60_MOSI_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_ENC28J60_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &buscfg, 1));

    /* ENC28J60 ethernet driver is based on spi driver */
    spi_device_interface_config_t devcfg = {
        .command_bits = 3,
        .address_bits = 5,
        .mode = 0,
        .clock_speed_hz = CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EXAMPLE_ENC28J60_CS_GPIO,
        .queue_size = 20,
        .cs_ena_posttrans = enc28j60_cal_spi_cs_hold_time(CONFIG_EXAMPLE_ENC28J60_SPI_CLOCK_MHZ),
    };

    spi_device_handle_t spi_handle = NULL;
    ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &devcfg, &spi_handle));

    eth_enc28j60_config_t enc28j60_config = ETH_ENC28J60_DEFAULT_CONFIG(CONFIG_EXAMPLE_ENC28J60_SPI_HOST, &devcfg);
    enc28j60_config.int_gpio_num = CONFIG_EXAMPLE_ENC28J60_INT_GPIO;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_enc28j60(&enc28j60_config, &mac_config);
    s_eth_mac = mac;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 0; // ENC28J60 doesn't support auto-negotiation
    phy_config.reset_gpio_num = -1;     // ENC28J60 doesn't have a pin to reset internal PHY
    esp_eth_phy_t *phy = esp_eth_phy_new_enc28j60(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    eth_config.check_link_period_ms = 200; /* default is 2000: faster link-up detection at boot */
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
    s_eth_handle = eth_handle;

    /* ENC28J60 doesn't burn any factory MAC address, we derive one from the ESP32's
       unique eFuse MAC to ensure each device gets a distinct Ethernet address. */
    uint8_t eth_mac[6];
    esp_efuse_mac_get_default(eth_mac);
    eth_mac[0] = 0x02; // locally administered, unicast
    mac->set_addr(mac, eth_mac);

    // Create default Ethernet station (eth) network interface
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, eth_netif_glue));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_event_handler, NULL));

    // Start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    s_eth_netif = eth_netif;

    // Restore static IP from NVS if previously configured
    ethernet_apply_saved_config();
}

esp_netif_t *ethernet_get_netif(void)
{
    return s_eth_netif;
}

bool ethernet_get_link(int *speed_mbps, bool *full_duplex)
{
    if (s_eth_handle == NULL || !ethernet_connected)
    {
        return false;
    }
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) != ESP_OK ||
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex) != ESP_OK)
    {
        return false;
    }
    if (speed_mbps != NULL)
    {
        *speed_mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
    }
    if (full_duplex != NULL)
    {
        *full_duplex = (duplex == ETH_DUPLEX_FULL);
    }
    return true;
}

// --- Static IP helpers ---

static bool ethernet_apply_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2)
{
    if (s_eth_netif == NULL)
    {
        ESP_LOGE(TAG, "Ethernet netif not initialized");
        return false;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "Failed to stop DHCP: %s", esp_err_to_name(err));
        return false;
    }

    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = ip;
    ip_info.netmask.addr = mask;
    ip_info.gw.addr = gw;

    err = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set IP info: %s", esp_err_to_name(err));
        esp_netif_dhcpc_start(s_eth_netif);
        return false;
    }

    ESP_LOGI(TAG, "Static IP applied: " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));

    if (dns1 != 0)
    {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.u_addr.ip4.addr = dns1;
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        ESP_LOGI(TAG, "DNS1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
    }

    if (dns2 != 0)
    {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.u_addr.ip4.addr = dns2;
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
        ESP_LOGI(TAG, "DNS2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
    }

    return true;
}

static bool ethernet_save_to_nvs(uint8_t mode, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ETH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_set_u8(h, "mode", mode);
    nvs_set_u32(h, "ip", ip);
    nvs_set_u32(h, "mask", mask);
    nvs_set_u32(h, "gw", gw);
    nvs_set_u32(h, "dns1", dns1);
    nvs_set_u32(h, "dns2", dns2);
    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void ethernet_apply_saved_config(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ETH_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return;
    }

    uint8_t mode = 0;
    nvs_get_u8(h, "mode", &mode);
    if (mode != 1)
    {
        nvs_close(h);
        return;
    }

    uint32_t ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    nvs_get_u32(h, "ip", &ip);
    nvs_get_u32(h, "mask", &mask);
    nvs_get_u32(h, "gw", &gw);
    nvs_get_u32(h, "dns1", &dns1);
    nvs_get_u32(h, "dns2", &dns2);
    nvs_close(h);

    if (ip == 0)
    {
        return;
    }

    ESP_LOGI(TAG, "Restoring static IP from NVS");
    ethernet_apply_static(ip, mask, gw, dns1, dns2);
}

// --- Public API ---

bool ethernet_set_static_ip(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns1, uint32_t dns2)
{
    bool ok = ethernet_apply_static(ip, mask, gw, dns1, dns2);
    if (!ok)
    {
        return false;
    }
    ethernet_save_to_nvs(1, ip, mask, gw, dns1, dns2);
    return true;
}

bool ethernet_clear_static_ip(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ETH_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    if (s_eth_netif != NULL)
    {
        // Clear the static IP so the framework recognizes the DHCP lease as new
        esp_netif_ip_info_t clear_info = {0};
        esp_netif_set_ip_info(s_eth_netif, &clear_info);

        err = esp_netif_dhcpc_start(s_eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        {
            ESP_LOGW(TAG, "Failed to restart DHCP: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "DHCP client restarted");
    }

    return true;
}
