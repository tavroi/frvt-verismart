cmake_minimum_required(VERSION 3.5)
project(nullImpl)

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
include_directories (${CMAKE_CURRENT_SOURCE_DIR}/../../../11/src/include ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/src/include)

# Configure built shared libraries in top-level lib directory
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)

# Build the shared libraries
add_library (frvt_11_twins_null_001 SHARED nullimplfrvt11_twins.cpp)
