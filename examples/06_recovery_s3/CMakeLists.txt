# 06_recovery_s3: an ESP32-S3 DevKitC-1 with nothing wired -- the chip's own
# temperature sensor and the on-board RGB LED -- updated over Wi-Fi through
# the recovery firmware (docs/PLANO.md 4.14).
cmake_minimum_required(VERSION 3.16)

# While the library is developed, BTPDevice, BTP and TinyShell come from
# sibling checkouts (see test_apps/btpdev_components.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/btpdev_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btpdev_06_recovery_s3)
