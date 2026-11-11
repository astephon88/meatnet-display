# Host build of the portable core (the firmware builds the same sources
# through firmware/components/core).
file(GLOB CORE_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
add_library(meatnet_core STATIC ${CORE_SRCS})
target_include_directories(meatnet_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_options(meatnet_core PRIVATE -Wall -Wextra)
