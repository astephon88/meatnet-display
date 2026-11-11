# The Linux unit (BlueZ over GDBus): app, field and standalone roles.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)

# The web page, embedded so the binary is self-contained.
set(PAGE ${CMAKE_CURRENT_SOURCE_DIR}/../www/index.html)
set(PAGE_CPP ${CMAKE_CURRENT_BINARY_DIR}/index_html.cpp)
add_custom_command(OUTPUT ${PAGE_CPP}
                   COMMAND ${CMAKE_COMMAND} -DIN=${PAGE} -DOUT=${PAGE_CPP} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
                   DEPENDS ${PAGE} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake)

add_executable(meatnet main.cpp central.cpp gdbus_util.cpp http.cpp peripheral.cpp host_transport.cpp ${PAGE_CPP})
target_link_libraries(meatnet meatnet_core PkgConfig::GIO pthread)
target_compile_options(meatnet PRIVATE -Wall -Wextra)
