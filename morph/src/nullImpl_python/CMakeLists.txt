cmake_minimum_required(VERSION 3.12)
project(nullImpl_python)

set(CMAKE_BUILD_TYPE Release)

execute_process(
    COMMAND python3 -m pybind11 --includes
    OUTPUT_VARIABLE PYBIND_INC
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND python3-config --ldflags --embed
    OUTPUT_VARIABLE PYTHON_LIB
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 ${PYBIND_INC} -Wno-attributes")
include_directories (${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/src/include)

# Configure built shared libraries in top-level lib directory
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

# We use MODULE for python extensions (equivalent to -shared)
add_library(frvt_morph_null-python_001 SHARED nullimplfrvtmorph.cpp)
target_link_libraries (frvt_morph_null-python_001 ${PYTHON_LIB})
