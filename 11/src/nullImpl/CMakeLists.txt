cmake_minimum_required(VERSION 3.5)
project(nullImpl)

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -Wall")
include_directories (${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/src/include)

# Configure built shared libraries in top-level lib directory
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)
file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

set(FRVT_PROVIDER "nxtgn" CACHE STRING "Provider name used in libfrvt_11_<provider>_<sequence>.so")
set(FRVT_SEQUENCE "001" CACHE STRING "Three digit submission sequence")

# Build the shared libraries
add_library (frvt11_submission SHARED nullimplfrvt11.cpp)
set_target_properties(frvt11_submission PROPERTIES
    OUTPUT_NAME "frvt_11_${FRVT_PROVIDER}_${FRVT_SEQUENCE}"
    PREFIX "lib")
