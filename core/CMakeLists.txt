# Host build of the portable core (the firmware builds the same sources
# through firmware/components/core, with ESP-IDF's own cJSON).
project(meatnet_core_lib C CXX)
file(GLOB CORE_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
# cJSON 1.7.19 (MIT), the version ESP-IDF 5.5 ships; host builds only.
add_library(cjson STATIC ${CMAKE_CURRENT_LIST_DIR}/third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_LIST_DIR}/third_party/cjson)
add_library(meatnet_core STATIC ${CORE_SRCS})
target_include_directories(meatnet_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(meatnet_core PUBLIC cjson)
target_compile_options(meatnet_core PRIVATE -Wall -Wextra)
