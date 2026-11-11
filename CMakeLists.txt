# Host build: the portable core and its tests (later also the Linux units).
# The ESP32 firmware is a separate ESP-IDF project in firmware/.
#   cmake -S . -B build && cmake --build build -j && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(meatnet_host C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(core)
enable_testing()
add_subdirectory(tests)

add_executable(screen_preview tools/screen_preview.cpp)
target_link_libraries(screen_preview meatnet_core)
target_compile_definitions(screen_preview PRIVATE MEATNET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

# The Linux unit: only where GLib's GIO (D-Bus) is available.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(GIO_PROBE QUIET gio-2.0)
endif()
if(GIO_PROBE_FOUND)
    add_subdirectory(linux)
endif()
