# 01_minimal: the smallest BTPDevice. Open TraceView, pick the device by name,
# open its terminal and type `help` or `btp -status`.
cmake_minimum_required(VERSION 3.16)

# While the library is developed, BTPDevice, BTP and TinyShell come from
# sibling checkouts (see test_apps/btpdev_components.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/btpdev_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(btpdev_01_minimal)
