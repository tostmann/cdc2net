# M4 (part 1) — pipe + onboarding + mDNS + WebUI status & OTA.
#   usb / usb_host_cdc_acm : USB host + vendored CDC-ACM driver (the CUL)
#   usb_host_{ftdi,ch34x,cp210x}_vcp : vendored VCP drivers for foreign
#                            USB-serial bridges — proper framing (FTDI
#                            status-byte strip) + baud control
#   improv_wifi_busware    : vendored local component (Improv-Serial)
#   mdns                   : vendored local component (hostname only)
#   esp_http_server        : WebUI; esp_http_client + esp-tls + json: ota_check
#   app_update + esp_app_format : OTA flash (esp_ota_*, esp_app_get_description)
idf_component_register(
    SRCS         "main.c" "bridge.c" "source_usb.c" "sink_tcp.c"
                 "net.c" "improv_glue.cpp" "mdns_glue.c" "config.c"
                 "log_buffer.c" "ota_check.c" "webui.c" "health.c"
                 "serialcfg.c"
    INCLUDE_DIRS "."
    REQUIRES     usb usb_host_cdc_acm
                 usb_host_ftdi_vcp usb_host_ch34x_vcp usb_host_cp210x_vcp
                 esp_wifi esp_event esp_netif nvs_flash lwip
                 driver esp_timer
                 improv_wifi_busware mdns
                 esp_http_server esp_http_client esp-tls json
                 app_update esp_app_format esp_https_ota
)
