cmake_minimum_required(VERSION 3.20)
project(aircast_receiver C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED
    gtk4
    gstreamer-1.0
    gstreamer-sdp-1.0
    gstreamer-webrtc-1.0
    json-glib-1.0
    libsoup-3.0
)

add_executable(aircast-receiver main.c)
# style.css.h is #included by main.c and lives beside it.
target_include_directories(aircast-receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS_INCLUDE_DIRS})
target_link_directories(aircast-receiver PRIVATE ${DEPS_LIBRARY_DIRS})
target_link_libraries(aircast-receiver PRIVATE ${DEPS_LIBRARIES})
target_compile_options(aircast-receiver PRIVATE ${DEPS_CFLAGS_OTHER})
