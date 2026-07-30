// SPDX-License-Identifier: GPL-2.0-or-later
//
// net_eth.c — W5500 SPI Ethernet bring-up for CDC2NET.
//
// Uses the in-core esp_eth W5500 driver (IDF 5.5.x: components/esp_eth/src/spi/
// w5500; needs CONFIG_ETH_USE_SPI_ETHERNET + CONFIG_ETH_SPI_ETHERNET_W5500).
// SPI pin map defaults to the EULFW32 C6 EUL/TUL generation; override per
// carrier board with -D flags.  An absent W5500 makes esp_eth_driver_install
// fail (it reads the chip version) → logged and skipped, WiFi unaffected.

#include "net.h"        // net_note_uplink_up()
#include "net_eth.h"
#include "config.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_phy.h"
#include "esp_eth_netif_glue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "net-eth";

// ── W5500 wiring (EULFW32 C6 EUL/TUL generation; override per carrier -D) ──
#ifndef W5500_SPI_HOST
#define W5500_SPI_HOST    SPI2_HOST
#endif
#ifndef W5500_SCLK
#define W5500_SCLK        19
#endif
#ifndef W5500_MISO
#define W5500_MISO        20
#endif
#ifndef W5500_MOSI
#define W5500_MOSI        21
#endif
#ifndef W5500_CS
#define W5500_CS          17
#endif
#ifndef W5500_INT                  // -1 → poll mode (poll_period_ms)
#define W5500_INT         0
#endif
#ifndef W5500_RST                  // -1 → no firmware-driven PHY reset
#define W5500_RST         -1
#endif
#ifndef W5500_CLOCK_MHZ
#define W5500_CLOCK_MHZ   20
#endif

static esp_netif_t      *s_eth_netif;
static esp_eth_handle_t  s_eth;
static volatile bool     s_eth_up;
static char              s_eth_ip[16] = "0.0.0.0";
static char              s_eth_gw[16] = "0.0.0.0";

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "W5500 link up");
#if defined(CONFIG_LWIP_IPV6)
        // Bring up IPv6 on the backbone netif (link-local + SLAAC).  Needed when
        // this netif is the OpenThread BR backbone (RS/ND6/MLD egress).
        if (s_eth_netif) esp_netif_create_ip6_linklocal(s_eth_netif);
#endif
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 link down");
        s_eth_up = false;
        snprintf(s_eth_ip, sizeof(s_eth_ip), "0.0.0.0");
        snprintf(s_eth_gw, sizeof(s_eth_gw), "0.0.0.0");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "W5500 started");
        break;
    case ETHERNET_EVENT_STOP:
        s_eth_up = false;
        break;
    default:
        break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_eth_ip, sizeof(s_eth_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    snprintf(s_eth_gw, sizeof(s_eth_gw), IPSTR, IP2STR(&ev->ip_info.gw));
    s_eth_up = true;
    ESP_LOGI(TAG, "W5500 got IP %s gw %s", s_eth_ip, s_eth_gw);
    // Wired uplink is up — retire a captive AP that a cable-less boot raised.
    net_note_uplink_up();
}

#if defined(CONFIG_LWIP_IPV6)
// IPv6 address acquired on the backbone netif (link-local or SLAAC global).
static void eth_got_ip6_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip6_t *ev = (const ip_event_got_ip6_t *)data;
    ESP_LOGI(TAG, "W5500 got IPv6 " IPV6STR, IPV62STR(ev->ip6_info.ip));
}
#endif

esp_err_t net_eth_init(void)
{
    const spi_bus_config_t buscfg = {
        .miso_io_num   = W5500_MISO,
        .mosi_io_num   = W5500_MOSI,
        .sclk_io_num   = W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t err = spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_initialize failed (%s) — ethernet disabled", esp_err_to_name(err));
        return err;
    }
    // The SPI-Ethernet module is interrupt-driven; install the GPIO ISR service
    // (idempotent — INVALID_STATE means someone already installed it).
    gpio_install_isr_service(0);

    spi_device_interface_config_t devcfg = {
        .mode           = 0,
        .clock_speed_hz = W5500_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num   = W5500_CS,
        .queue_size     = 20,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &devcfg);
    w5500_config.int_gpio_num = W5500_INT;
#if W5500_INT < 0
    w5500_config.poll_period_ms = 10;
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = 1;
    phy_config.reset_gpio_num = W5500_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        // Most common cause: no W5500 on the FPC header (chip-version read fails).
        ESP_LOGW(TAG, "W5500 not detected (%s) — ethernet disabled, WiFi unaffected",
                 esp_err_to_name(err));
        if (mac) mac->del(mac);
        if (phy) phy->del(phy);
        spi_bus_free(W5500_SPI_HOST);
        s_eth = NULL;
        return err;
    }

    // W5500 has no own MAC in this driver mode — derive one from the efuse.
    uint8_t macaddr[6];
    esp_read_mac(macaddr, ESP_MAC_ETH);
    esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, macaddr);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               eth_got_ip_handler, NULL));
#if defined(CONFIG_LWIP_IPV6)
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6,
                                               eth_got_ip6_handler, NULL));
#endif

    // Static IP (optional) — applied before esp_eth_start, mirroring net.c's
    // STA path.  Otherwise the eth netif's DHCP client runs on link-up.  Either
    // way IP_EVENT_ETH_GOT_IP fires and populates s_eth_ip/s_eth_gw.
    cdc2net_cfg_t cfg;
    config_load(&cfg);
    if (cfg.eth_static_ip && cfg.eth_ip[0] && cfg.eth_mask[0] && cfg.eth_gw[0]) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_str_to_ip4(cfg.eth_ip,   &ip.ip);
        esp_netif_str_to_ip4(cfg.eth_mask, &ip.netmask);
        esp_netif_str_to_ip4(cfg.eth_gw,   &ip.gw);
        esp_netif_dhcpc_stop(s_eth_netif);
        if (esp_netif_set_ip_info(s_eth_netif, &ip) == ESP_OK) {
            if (cfg.eth_dns[0]) {
                esp_netif_dns_info_t dns = {0};
                esp_netif_str_to_ip4(cfg.eth_dns, &dns.ip.u_addr.ip4);
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
            }
            ESP_LOGW(TAG, "static IP %s/%s gw %s", cfg.eth_ip, cfg.eth_mask, cfg.eth_gw);
        } else {
            ESP_LOGE(TAG, "eth static IP set failed — falling back to DHCP");
            esp_netif_dhcpc_start(s_eth_netif);
        }
    }

    ESP_ERROR_CHECK(esp_eth_start(s_eth));
    ESP_LOGI(TAG, "W5500 ethernet started (host=%d sclk=%d miso=%d mosi=%d cs=%d int=%d)",
             (int)W5500_SPI_HOST, W5500_SCLK, W5500_MISO, W5500_MOSI, W5500_CS, W5500_INT);
    return ESP_OK;
}

bool net_eth_is_up(void)         { return s_eth_up; }
bool net_eth_present(void)       { return s_eth != NULL; }   // W5500 detected + driver up
const char *net_eth_ip_str(void) { return s_eth_ip; }
const char *net_eth_gw_str(void) { return s_eth_gw; }
esp_netif_t *net_eth_netif(void) { return s_eth_netif; }     // backbone for MISSION_TBR (OTBR)
