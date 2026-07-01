# M4 (part 1) — pipe + onboarding + mDNS + WebUI status & OTA.
#   improv_wifi_busware    : vendored local component (Improv-Serial)
#   mdns                   : vendored local component (hostname only)
#   esp_http_server        : WebUI; esp_http_client + esp-tls + json: ota_check
#   app_update + esp_app_format : OTA flash (esp_ota_*, esp_app_get_description)
#
# Source-agnostic core (always built) + a per-mission data source gated on the
# Kconfig choice CDC2NET_SOURCE_* (see Kconfig.projbuild / docs/merge-targets.md).
# Each mission selects exactly ONE source; its SRCS + REQUIRES come in via the
# matching if() block, so a non-USB target never drags in the USB-host stack.
set(cdc2net_srcs
    "main.c" "bridge.c" "sink_tcp.c"
    "net.c" "improv_glue.cpp" "mdns_glue.c" "config.c"
    "log_buffer.c" "ota_check.c" "webui.c" "health.c"
    "serialcfg.c")
set(cdc2net_reqs
    esp_wifi esp_event esp_netif nvs_flash lwip
    driver esp_timer
    improv_wifi_busware mdns
    esp_http_server esp_http_client esp-tls json
    app_update esp_app_format esp_https_ota)

if(CONFIG_CDC2NET_SOURCE_USB)
    #   usb / usb_host_cdc_acm : USB host + vendored CDC-ACM driver (the CUL)
    #   usb_host_{ftdi,ch34x,cp210x}_vcp : vendored VCP drivers for foreign
    #     USB-serial bridges — FTDI status-byte strip + baud control.  These
    #     vendored components must self-target-guard (USB-OTG = S2/S3/P4 only)
    #     before a non-OTG mission builds — see docs/merge-targets.md §3.2.
    list(APPEND cdc2net_srcs "source_usb.c")
    list(APPEND cdc2net_reqs usb usb_host_cdc_acm
                 usb_host_ftdi_vcp usb_host_ch34x_vcp usb_host_cp210x_vcp)
endif()

if(CONFIG_CDC2NET_SOURCE_UART)
    # EUL/TUL onboard radio (TCM515) over a hardware UART — byte-transparent.
    # The UART + GPIO APIs come from `driver`, already in the core REQUIRES.
    list(APPEND cdc2net_srcs "source_uart.c")
endif()
# Future mission: if(CONFIG_CDC2NET_SOURCE_THREAD) -> source_thread.c + openthread

# W5500 SPI Ethernet (line-wide on the C6 EUL/TUL generation; FPC add-on).
if(CONFIG_CDC2NET_ETH_W5500)
    list(APPEND cdc2net_srcs "net_eth.c")
    list(APPEND cdc2net_reqs esp_eth)
endif()

# M24C32 I2C config EEPROM (C6 EUL/TUL carrier; bridge PCB pads unpopulated).
# New esp_driver_i2c master API; GPIO for the WE line comes via `driver`.
if(CONFIG_CDC2NET_EEPROM_M24C32)
    list(APPEND cdc2net_srcs "eeprom_m24c32.c")
    list(APPEND cdc2net_reqs esp_driver_i2c)
endif()

# Experimental on-device Thread Border Router (C6 native radio) on the W5500
# backbone, alongside the bridge.  Gated on CDC2NET_MISSION_TBR (default n) —
# pulls OpenThread + full lwIP-IPv6 forwarding + mbedtls in (see
# sdkconfig.defaults.tbr).  `vfs` provides esp_vfs_eventfd for the OT mainloop.
if(CONFIG_CDC2NET_MISSION_TBR)
    list(APPEND cdc2net_srcs "mission_tbr.c")
    list(APPEND cdc2net_reqs openthread vfs)
endif()

idf_component_register(
    SRCS         ${cdc2net_srcs}
    INCLUDE_DIRS "."
    REQUIRES     ${cdc2net_reqs}
)
