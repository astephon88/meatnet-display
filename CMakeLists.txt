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
