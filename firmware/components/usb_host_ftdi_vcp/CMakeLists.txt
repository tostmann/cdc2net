# Vendored from esp-usb host/class/cdc/usb_host_ftdi_vcp (v2.1.1).
# Local change vs upstream: explicit REQUIRES so the sibling vendored CDC-ACM
# driver resolves at the CMake level (no component-manager registry lookup).
# Target guard: USB-OTG host exists only on S2/S3/P4.  On non-OTG SoCs (the
# C3/C6 EUL/TUL line) register an EMPTY component — the vendored dir stays a
# valid component but drags in nothing and does not REQUIRE the target-excluded
# CDC-ACM driver (which would fail dependency resolution).  docs/merge-targets §3.2.
# Guard on IDF_TARGET, NOT CONFIG_* (undefined during early component expansion).
if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    idf_component_register(SRCS "usb_host_ftdi_vcp.c"
                           INCLUDE_DIRS "include"
                           REQUIRES usb_host_cdc_acm)
else()
    idf_component_register()
endif()
