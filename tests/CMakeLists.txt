# C++ host tests for core/ (doctest). The Python tests beside them go away
# once the port is complete.
file(GLOB TEST_SRCS ${CMAKE_CURRENT_LIST_DIR}/*.cpp)
add_executable(core_tests ${TEST_SRCS})
target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party)
target_link_libraries(core_tests PRIVATE meatnet_core)
add_test(NAME core_tests COMMAND core_tests)
