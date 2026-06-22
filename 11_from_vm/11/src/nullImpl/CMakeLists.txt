cmake_minimum_required(VERSION 3.5)
project(nullImpl)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -Wall")

# FRVT and common headers
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/src/include
)

# ONNX Runtime headers — expected at 11/include/onnxruntime/
set(ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include/onnxruntime")
if(NOT EXISTS "${ORT_ROOT}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "ONNX Runtime headers not found at ${ORT_ROOT}/onnxruntime_cxx_api.h\n"
        "Download onnxruntime-linux-x64-1.18.1.tgz from GitHub releases and run:\n"
        "  mkdir -p 11/include/onnxruntime\n"
        "  tar -xzf onnxruntime-linux-x64-1.18.1.tgz\n"
        "  cp onnxruntime-linux-x64-1.18.1/include/* 11/include/onnxruntime/\n"
        "  cp onnxruntime-linux-x64-1.18.1/lib/libonnxruntime.so.1.18.1 11/lib/\n"
        "  ln -sf libonnxruntime.so.1.18.1 11/lib/libonnxruntime.so")
endif()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/onnxruntime)

# ONNX Runtime library — expected at 11/lib/
set(ORT_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lib")
find_library(ORT_LIB onnxruntime HINTS ${ORT_LIB_DIR} NO_DEFAULT_PATH)
if(NOT ORT_LIB)
    message(FATAL_ERROR
        "libonnxruntime.so not found in ${ORT_LIB_DIR}\n"
        "Place libonnxruntime.so (and libonnxruntime.so.1.18.1) in 11/lib/")
endif()

set(FRVT_PROVIDER "nxtgn" CACHE STRING "Provider name")
set(FRVT_SEQUENCE "001"   CACHE STRING "Three-digit submission sequence")

# Output directory is 11/lib so the .so lands next to libonnxruntime.so
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)
file(MAKE_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

add_library(frvt11_submission SHARED nullimplfrvt11.cpp)
set_target_properties(frvt11_submission PROPERTIES
    OUTPUT_NAME "frvt_11_${FRVT_PROVIDER}_${FRVT_SEQUENCE}"
    PREFIX      "lib")
target_link_libraries(frvt11_submission ${ORT_LIB})
