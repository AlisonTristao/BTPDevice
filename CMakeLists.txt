# BTPDevice -- ESP-IDF component.
#
# Dependencies are the BTP and TinyShell components (both ESP-IDF components
# on their own since BTP 2.49.0 / TinyShell 2.1.0, docs/PLANO.md D15). While
# the library is being built they come from sibling checkouts listed in the
# project's EXTRA_COMPONENT_DIRS (see test_apps/), which is why the names
# below are the directory names "BTP" and "TinyShell". Packaging through the
# component manager is validated in Phase 7.
#
# Host tests do not go through this file: test/host/CMakeLists.txt compiles
# the portable sources directly.

if(NOT ESP_PLATFORM)
    message(FATAL_ERROR
        "BTPDevice is an ESP-IDF component. For the host tests use "
        "test/host/CMakeLists.txt.")
endif()

# include/btpdev/version.h and idf_component.yml each carry the version; the
# component manager reads the YAML, the code reads the header. Refuse to
# build if they disagree.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/btpdev/version.h" _btpdev_version_line
     REGEX "#define BTPDEV_VERSION_STRING")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _btpdev_header_version "${_btpdev_version_line}")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" _btpdev_yml_line
     REGEX "^version:")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _btpdev_yml_version "${_btpdev_yml_line}")
if(NOT _btpdev_header_version STREQUAL _btpdev_yml_version)
    message(FATAL_ERROR
        "BTPDevice version mismatch: include/btpdev/version.h says "
        "${_btpdev_header_version}, idf_component.yml says ${_btpdev_yml_version}")
endif()

set(btpdev_srcs
    "src/core/BtpDevice.cpp"
    "src/core/ConfigCheck.cpp"
    "src/core/DeviceCore.cpp"
    "src/core/Error.cpp"
    "src/core/FrameQueue.cpp"
    "src/core/Identity.cpp"
    "src/core/Memory.cpp"
    "src/core/RuntimeErrors.cpp"
    "src/core/StreamFraming.cpp"
    "src/protocol/ShellText.cpp"
    "src/protocol/Terminal.cpp"
    "src/protocol/TopicRegistry.cpp"
    "src/transport/BleLink.cpp"
    "src/transport/EspNowLink.cpp"
    "src/transport/TcpLink.cpp"
    "src/transport/UsbSerialLink.cpp"
    "src/transport/WifiCore.cpp"
    "src/transport/WifiLogic.cpp"
    "src/transport/WifiNetworks.cpp"
    "src/transport/WifiStation.cpp")

# `bt` unconditionally: CONFIG_* values are not known yet when ESP-IDF
# expands component requirements, so an if(CONFIG_BT_NIMBLE_ENABLED) here is
# always false. BleLink.cpp compiles to a stub when NimBLE is off. esp_tinyusb
# and mdns come from idf_component.yml (esp_tinyusb only on chips with USB OTG).
set(btpdev_priv_requires log esp_timer esp_event esp_netif esp_wifi nvs_flash lwip bt esp_app_format)

idf_component_register(
    SRCS ${btpdev_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES BTP TinyShell
    PRIV_REQUIRES ${btpdev_priv_requires})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
