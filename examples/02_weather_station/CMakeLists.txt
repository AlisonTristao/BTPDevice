# 02_weather_station: sensors over Wi-Fi, bound to variables.
cmake_minimum_required(VERSION 3.16)

# While the library is developed, TraceKit, BTP and TinyShell come from
# sibling checkouts (see test_apps/tracekit_components.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../../test_apps/tracekit_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tracekit_02_weather_station)
