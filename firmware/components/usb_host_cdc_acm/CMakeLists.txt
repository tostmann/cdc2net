# 1. IDF version >= 6.0 does not have usb component: usb from IDF component manager will be used
# 2. For linux target, we can't use IDF component manager to get usb component, we need to add it 'the old way'
#    with EXTRA_COMPONENT_DIRS because mocking of managed components is not supported yet.
#    This is acceptable workaround for testing.
# Target guard (CDC2NET merge): USB-OTG host exists only on S2/S3/P4.  On a
# non-OTG SoC (C3/C6 EUL/TUL line) register an EMPTY component — the vendored
# dir stays valid but compiles no USB-host sources and pulls no `usb` requires.
# This replaces the upstream `targets:` manifest field (removed) which, for a
# LOCAL component, hard-errors instead of excluding.  See docs/merge-targets §3.2.
# Guard on IDF_TARGET, NOT CONFIG_* — CONFIG_ vars are undefined during the early
# component-expansion phase that fixes the REQUIRES graph, so a CONFIG_ guard
# would drop `usb` from REQUIRES on S2/S3/P4 too (usb_host.h then not found).
if(IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    set(requires "")
    if((${IDF_VERSION_MAJOR} LESS 6) OR ("${IDF_TARGET}" STREQUAL "linux"))
        list(APPEND requires usb)
    endif()

    idf_component_register(SRCS
                            "cdc_acm_host.c"                # Contains driver and transfers code
                            "cdc_host_descriptor_parsing.c" # Only descriptor parsing code, no CDC device handling
                            "cdc_host_acm_compliant.c"      # Implementation of CDC ACM compliant functions
                            "cdc_host_ops.c"                # Implementation of CDC ACM host operations
                           INCLUDE_DIRS "include" "interface"
                           PRIV_INCLUDE_DIRS "private_include" "include/esp_private"
                           REQUIRES "${requires}"
                           )
else()
    idf_component_register()
endif()
