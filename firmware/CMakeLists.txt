cmake_minimum_required(VERSION 3.16)
# NOTE: do NOT set(EXTRA_COMPONENT_DIRS ...) here — PlatformIO uses it to map
# this project's src/ dir onto the IDF `main` component, and overriding it
# drops `main` ("Couldn't find the main target").  Extra components are
# vendored under components/ (usb_host_cdc_acm, improv_wifi_busware) and are
# auto-discovered, so no override is needed.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cdc2net)
