cmake_minimum_required(VERSION 3.20)
project(aircast_receiver C)

set(CMAKE_C_STANDARD 11)

# Stamped from the git tag by CI (-DAIRCAST_VERSION=1.2.3). A build that is not
# a tag stamps 0.0.0, which every real release compares newer than.
set(AIRCAST_VERSION "0.0.0" CACHE STRING "version compiled into the binary")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED
    gtk4
    gstreamer-1.0
    gstreamer-sdp-1.0
    gstreamer-webrtc-1.0
    json-glib-1.0
    libsoup-3.0
    # Ed25519 + BLAKE2b for the update manifest signature. Windows CNG is not an
    # alternative: bcrypt.h defines Curve25519 for ECDH only, with no EdDSA
    # algorithm identifier.
    libsodium
)

add_executable(aircast-receiver main.c update_check.c)
target_compile_definitions(aircast-receiver PRIVATE AIRCAST_VERSION="${AIRCAST_VERSION}")
# style.css.h is #included by main.c and lives beside it.
target_include_directories(aircast-receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS_INCLUDE_DIRS})
target_link_directories(aircast-receiver PRIVATE ${DEPS_LIBRARY_DIRS})
target_link_libraries(aircast-receiver PRIVATE ${DEPS_LIBRARIES})
target_compile_options(aircast-receiver PRIVATE ${DEPS_CFLAGS_OTHER})
