cmake_minimum_required(VERSION 3.20)
project(cruisn_pc LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/utf-8 /permissive- /W3 /wd4244 /wd4267 /wd4146 /wd4018)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # symbols for the development profiler (no runtime cost); whole-program optimisation was measured and gave nothing
  add_compile_options($<$<CONFIG:Release>:/Oi> $<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

option(C3X_PROFILE "count executed opcodes (development)" OFF)
if(C3X_PROFILE)
  add_compile_definitions(C3X_PROFILE)
endif()

add_library(cpu_c3x STATIC src/cpu/tms320c3x/tms320c3x.cpp)
target_include_directories(cpu_c3x PUBLIC src)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
add_library(machine STATIC src/machine/midvunit.cpp src/machine/cmos.cpp src/machine/rom_patches.cpp src/machine/dcs_worker.cpp src/machine/telemetry.cpp src/video/png_io.cpp src/video/texrepl.cpp src/audio/dcs.cpp)
target_include_directories(machine PUBLIC src)
target_link_libraries(machine PUBLIC cpu_c3x cpu_adsp miniz)
add_executable(cruisn_headless src/main_headless.cpp src/input/ffb_modern.cpp)
target_link_libraries(cruisn_headless PRIVATE machine)

add_library(volk STATIC third_party/volk/volk.c)
target_include_directories(volk PUBLIC third_party/volk third_party/vulkan/include)
target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)

add_library(imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/imgui_impl_win32.cpp third_party/imgui/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS=0 IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

add_executable(cruisn_usa WIN32
  src/main_win.cpp
  src/platform/audio_wasapi.cpp src/platform/splash.cpp src/platform/stall_watch.cpp src/platform/vblank_clock.cpp
  src/video/gl_backend.cpp src/video/vk_backend.cpp
  src/config/settings.cpp
  src/input/devices.cpp src/input/controls.cpp src/input/ffb_modern.cpp
  src/outputs/outputs.cpp
  src/launcher/launcher.cpp
  assets/cruisn.rc)
target_include_directories(cruisn_usa PRIVATE assets)
target_link_libraries(cruisn_usa PRIVATE machine volk imgui)

# Every build puts the program into two places: bin/<config>/ (with the development tools, symbols and whatever the tests
# leave there) and bin/<config>/RELEASE CLEAN/, which holds only what is handed out: the program and its documents.
set(RELEASE_CLEAN "$<TARGET_FILE_DIR:cruisn_usa>/RELEASE CLEAN")
add_custom_command(TARGET cruisn_usa POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_CLEAN}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cruisn_usa>" "${RELEASE_CLEAN}/"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/docs/release/README.txt" "${CMAKE_SOURCE_DIR}/docs/release/LICENSES.txt" "${RELEASE_CLEAN}/"
  VERBATIM)

add_library(cpu_adsp STATIC src/cpu/adsp2100/adsp2100.cpp)
target_include_directories(cpu_adsp PUBLIC src)

# development tool: disassembles the game ROM to find patch sites
add_executable(romdis tools/romdis/romdis.cpp src/cpu/tms320c3x/tms320c3x_dasm.cpp)
target_include_directories(romdis PRIVATE tools/romdis/stub)
target_link_libraries(romdis PRIVATE miniz)
