# The portable core (../../../core): protocol, Node, split, ... It has no
# ESP-IDF dependencies; the host build (top-level CMakeLists.txt) builds and
# tests the same sources.
set(CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../core)
file(GLOB CORE_SRCS ${CORE_DIR}/src/*.cpp)
idf_component_register(SRCS ${CORE_SRCS}
                       INCLUDE_DIRS ${CORE_DIR}/include
                       REQUIRES json)
