cmake_minimum_required(VERSION 3.10)
project(SimpleWebServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -O2 -pthread)

include_directories(include)

file(GLOB SOURCES src/*.cpp)
add_executable(server ${SOURCES})

target_link_libraries(server pthread)
