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
    esp_http_server esp_http_client esp-tls
    app_update esp_app_format esp_https_ota)

# cJSON (ota_check manifest parse): in-tree component `json` through IDF 5.x;
# moved to the component registry in IDF 6 (espressif/cjson → component name
# espressif__cjson, pulled in via idf_component.yml with an idf_version rule).
# Verified: the S31/IDF-master build resolves cjson 1.7.x from the registry,
# the pinned PlatformIO/IDF 5.5 builds keep using the in-tree `json`.
if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND cdc2net_reqs json)
else()
    list(APPEND cdc2net_reqs espressif__cjson)
endif()

if(CONFIG_CDC2NET_SOURCE_USB)
    #   usb / usb_host_cdc_acm : USB host + vendored CDC-ACM driver (the CUL)
    #   usb_host_{ftdi,ch34x,cp210x}_vcp : vendored VCP drivers for foreign
    #     USB-serial bridges — FTDI status-byte strip + baud control.  These
    #     vendored components must self-target-guard (USB-OTG = S2/S3/P4 only)
    #     before a non-OTG mission builds — see docs/merge-targets.md §3.2.
    # cul_dfu.c haengt am selben USB-Wirt: es flasht den angesteckten
    # AVR-Stick ueber sein DFU-Bootladerprofil. Nur sinnvoll, wo es
    # ueberhaupt einen USB-Wirt gibt.
    list(APPEND cdc2net_srcs "source_usb.c" "hubprobe.c" "cul_dfu.c")
    list(APPEND cdc2net_reqs usb usb_host_cdc_acm
                 usb_host_ftdi_vcp usb_host_ch34x_vcp usb_host_cp210x_vcp)
endif()

if(CONFIG_CDC2NET_SOURCE_USB_CHERRY)
    # Same source contract as source_usb.c, on the CherryUSB stack: it brings
    # its own CDC-ACM/FTDI/CH34x/CP210x class drivers, so none of the vendored
    # *_vcp components (nor esp-usb's cdc_acm) are built on this path.  The
    # component lives in components/cherryusb (trimmed 1.6.1 + local hub fix,
    # see its VENDORING.md); it used to come from the registry.
    list(APPEND cdc2net_srcs "source_usb_cherry.c")
endif()

# REQUIRES must be gated on IDF_TARGET, not on CONFIG_*: component requirements
# are expanded before Kconfig values exist, so a CONFIG_-guarded list(APPEND
# cdc2net_reqs …) silently never reaches idf_component_register() — the SRCS
# above do get the guard, which is why a CONFIG-gated source compiled while its
# component's headers were missing.  While cherryusb came from the registry the
# manifest's `rules:` entry made the component manager add the requirement, so
# this went unnoticed; vendoring removed that path.  Same target set as the old
# manifest rule.  Gating wider than the mission costs one translation unit, not
# zero: the component's own CMakeLists registers on ESP_PLATFORM regardless, so
# osal/idf/usb_osal_idf.c compiles even on the UART-only C6 mission (visible in
# the build log).  Nothing of it is referenced there, so none of it links.
if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3"
   OR IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND cdc2net_reqs cherryusb)
endif()

if(CONFIG_CDC2NET_SOURCE_UART)
    # EUL/TUL onboard radio (TCM515) over a hardware UART — byte-transparent.
    # The UART + GPIO APIs come from `driver`, already in the core REQUIRES.
    list(APPEND cdc2net_srcs "source_uart.c")
endif()
# Future mission: if(CONFIG_CDC2NET_SOURCE_THREAD) -> source_thread.c + openthread

# Companion-radio flashing (ESP Thread-BR / Zigbee-GW board: the S3 host writes
# the H2's firmware over the same inter-chip UART) — see radio_flash.h.  The
# radio image is NOT embedded in the app: it lives in the `radio_fw` partition
# (partitions_ota.csv), read back through esp_partition.  EMBED_FILES would be
# the obvious route but PlatformIO builds `main` with SCons and never generates
# the embed .S; the partition also keeps ~720 KB out of both OTA slots and lets
# app and radio be updated independently.  esp-serial-flasher arrives as a
# managed dependency (idf_component.yml) and reaches REQUIRES on its own;
# esp_app_format (core REQUIRES) provides esp_app_desc_t.
if(CONFIG_CDC2NET_RADIO_FLASH)
    list(APPEND cdc2net_srcs "radio_flash.c" "radio_md5.c")
endif()
list(APPEND cdc2net_reqs esp_partition mbedtls)

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
