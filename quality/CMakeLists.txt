cmake_minimum_required(VERSION 3.5)
project(FRVT_validation)
set(CMAKE_BUILD_TYPE Release)

# Build libraries
#add_subdirectory(src/nullImpl)

# Build testdriver
add_subdirectory(src/testdriver)
