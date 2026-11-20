# Shared by every project under test_apps/ and examples/ while the library is
# developed: TraceKit itself plus the BTP and TinyShell checkouts, expected as
# siblings of this repository (c:/git/TraceKit, c:/git/BTP, c:/git/TinyShell).
# Override with -DTRACEKIT_BTP_DIR=... / -DTRACEKIT_TINYSHELL_DIR=... when they
# live elsewhere (CI checks them out next to this repository too).
get_filename_component(_tracekit_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# ESP-IDF names a component after its folder, and the examples say
# `REQUIRES TraceKit`: the checkout must be called TraceKit.
get_filename_component(_tracekit_folder "${_tracekit_root}" NAME)
if(NOT _tracekit_folder STREQUAL "TraceKit")
    message(FATAL_ERROR "The library checkout is \"${_tracekit_folder}\": rename the folder to "
                        "TraceKit (ESP-IDF names the component after it; the examples require TraceKit).")
endif()
if(NOT DEFINED TRACEKIT_BTP_DIR)
    get_filename_component(TRACEKIT_BTP_DIR "${_tracekit_root}/../BTP" ABSOLUTE)
endif()
if(NOT DEFINED TRACEKIT_TINYSHELL_DIR)
    get_filename_component(TRACEKIT_TINYSHELL_DIR "${_tracekit_root}/../TinyShell" ABSOLUTE)
endif()
foreach(_dir "${TRACEKIT_BTP_DIR}" "${TRACEKIT_TINYSHELL_DIR}")
    if(NOT EXISTS "${_dir}/CMakeLists.txt")
        message(FATAL_ERROR "TraceKit dependency not found at ${_dir}")
    endif()
endforeach()
list(APPEND EXTRA_COMPONENT_DIRS
    "${_tracekit_root}" "${TRACEKIT_BTP_DIR}" "${TRACEKIT_TINYSHELL_DIR}")
