# Shared by every project under test_apps/ and examples/ while the library is
# developed: BTPDevice itself plus the BTP and TinyShell checkouts, expected as
# siblings of this repository (c:/git/BTPDevice, c:/git/BTP, c:/git/TinyShell).
# Override with -DBTPDEV_BTP_DIR=... / -DBTPDEV_TINYSHELL_DIR=... when they
# live elsewhere (CI checks them out next to this repository too).
get_filename_component(_btpdev_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED BTPDEV_BTP_DIR)
    get_filename_component(BTPDEV_BTP_DIR "${_btpdev_root}/../BTP" ABSOLUTE)
endif()
if(NOT DEFINED BTPDEV_TINYSHELL_DIR)
    get_filename_component(BTPDEV_TINYSHELL_DIR "${_btpdev_root}/../TinyShell" ABSOLUTE)
endif()
foreach(_dir "${BTPDEV_BTP_DIR}" "${BTPDEV_TINYSHELL_DIR}")
    if(NOT EXISTS "${_dir}/CMakeLists.txt")
        message(FATAL_ERROR "BTPDevice dependency not found at ${_dir}")
    endif()
endforeach()
list(APPEND EXTRA_COMPONENT_DIRS
    "${_btpdev_root}" "${BTPDEV_BTP_DIR}" "${BTPDEV_TINYSHELL_DIR}")
