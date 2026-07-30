cmake_minimum_required(VERSION 3.16)
# NOTE: do NOT set(EXTRA_COMPONENT_DIRS ...) here — PlatformIO uses it to map
# the project's src_dir (= main/, see platformio.ini) onto the IDF `main`
# component, and overriding it drops `main` ("Couldn't find the main target").
# App sources live in main/ (plain-IDF layout) so the same tree also builds
# with plain `idf.py` — the ESP32-S31 path (preview target, IDF master only):
#   idf.py --preview set-target esp32s31 && idf.py build
# Extra components are vendored under components/ (usb_host_cdc_acm,
# improv_wifi_busware) and are auto-discovered, so no override is needed.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cdc2net)
