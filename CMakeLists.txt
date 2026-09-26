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

idf_component_register(
    SRCS
        "src/core/BtpDevice.cpp"
        "src/core/ConfigCheck.cpp"
        "src/core/Error.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES BTP TinyShell
    PRIV_REQUIRES log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
