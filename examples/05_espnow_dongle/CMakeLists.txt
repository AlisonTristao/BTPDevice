# 05_espnow_dongle: ESP-NOW through the bally_dongle.
cmake_minimum_required(VERSION 3.16)

# While the library is developed, BTPDevice, BTP and TinyShell come from
# sibling checkouts (see test_apps/btpdev_components.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/btpdev_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btpdev_05_espnow_dongle)
