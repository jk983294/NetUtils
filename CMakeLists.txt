cmake_minimum_required(VERSION 3.5)

project(NetUtils)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# boost lib
find_package( Boost 1.61.0 COMPONENTS program_options )
include_directories(${Boost_INCLUDE_DIRS})


# SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -O3 -Wall -Werror")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -g -Wall -Werror")
message ("cxx Flags: " ${CMAKE_CXX_FLAGS})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include_directories(common)

add_subdirectory(common)
add_subdirectory(proxy)
add_subdirectory(balancer)
add_subdirectory(experiments)
