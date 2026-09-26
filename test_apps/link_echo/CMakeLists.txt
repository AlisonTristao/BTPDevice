# Hardware test for the links (docs/PLANO.md, Fase 2): every valid BTP frame
# received is sent back on the same link; tools/link_echo_test.py is the PC
# side. Pick the link in menuconfig ("Link echo") or with a PlatformIO env.
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/../btpdev_components.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(link_echo)
