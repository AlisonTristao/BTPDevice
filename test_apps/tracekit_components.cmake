# Shared by every project under test_apps/ and examples/: registers this
# repository as the TraceKit component. BTP and TinyShell are fetched from
# GitHub by the component manager (see idf_component.yml at the root).
#
# To build against local checkouts of BTP or TinyShell instead (when changing
# them together with TraceKit), pass -DTRACEKIT_BTP_DIR=<path> and/or
# -DTRACEKIT_TINYSHELL_DIR=<path>: components in EXTRA_COMPONENT_DIRS take
# precedence over the downloaded ones.
get_filename_component(_tracekit_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# ESP-IDF names a component after its folder, and the examples say
# `REQUIRES TraceKit`: the checkout must be called TraceKit.
get_filename_component(_tracekit_folder "${_tracekit_root}" NAME)
if(NOT _tracekit_folder STREQUAL "TraceKit")
    message(FATAL_ERROR "The library checkout is \"${_tracekit_folder}\": rename the folder to "
                        "TraceKit (ESP-IDF names the component after it; the examples require TraceKit).")
endif()
list(APPEND EXTRA_COMPONENT_DIRS "${_tracekit_root}")
foreach(_dir IN ITEMS ${TRACEKIT_BTP_DIR} ${TRACEKIT_TINYSHELL_DIR})
    if(NOT EXISTS "${_dir}/CMakeLists.txt")
        message(FATAL_ERROR "TraceKit: no CMakeLists.txt at ${_dir}")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${_dir}")
endforeach()
