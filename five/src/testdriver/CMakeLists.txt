set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -Wall -DNIST_EXTERN_FIVE_STRUCTS_VERSION -DNIST_EXTERN_API_VERSION -DNIST_EXTERN_FRVT_STRUCTS_VERSION")
include_directories (${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../../../common/src/include)

# Configure to put executable in top level bin directory
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Get library implementation name
set (FIVE_IMPL_LIB $ENV{FIVE_IMPL_LIB})

# Build executable link to dependent libraries
add_executable (validate_five ../../../common/src/util/util.cpp validate_five.cpp)
target_link_libraries (validate_five ${FIVE_IMPL_LIB})
