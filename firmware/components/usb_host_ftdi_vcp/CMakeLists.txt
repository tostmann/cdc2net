# Vendored from esp-usb host/class/cdc/usb_host_ftdi_vcp (v2.1.1).
# Local change vs upstream: explicit REQUIRES so the sibling vendored CDC-ACM
# driver resolves at the CMake level (no component-manager registry lookup).
idf_component_register(SRCS "usb_host_ftdi_vcp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES usb_host_cdc_acm)
