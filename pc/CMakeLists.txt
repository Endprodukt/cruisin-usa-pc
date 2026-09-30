cmake_minimum_required(VERSION 3.20)
project(cruisn_pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /W3 /wd4244 /wd4267 /wd4146 /wd4018)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_library(cpu_c3x STATIC src/cpu/tms320c3x/tms320c3x.cpp)
target_include_directories(cpu_c3x PUBLIC src)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
add_library(machine STATIC src/machine/midvunit.cpp)
target_include_directories(machine PUBLIC src)
target_link_libraries(machine PUBLIC cpu_c3x miniz)
add_executable(cruisn_headless src/main_headless.cpp)
target_link_libraries(cruisn_headless PRIVATE machine)

add_executable(cruisn_usa WIN32 src/main_win.cpp)
target_link_libraries(cruisn_usa PRIVATE machine)
