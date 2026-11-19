# cmake -DBIN=<file> -DLIMIT=<bytes> -P check_size.cmake
file(SIZE "${BIN}" _size)
math(EXPR _free "${LIMIT} - ${_size}")
if(_size GREATER LIMIT)
    message(FATAL_ERROR "recovery is ${_size} bytes, over the ${LIMIT}-byte recovery partition")
endif()
message(STATUS "recovery: ${_size} bytes, ${_free} free in the 1 MB partition")
