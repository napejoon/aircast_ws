cmake_minimum_required(VERSION 3.20)
project(kagami C)

set(CMAKE_C_STANDARD 11)

# Stamped from the git tag by CI (-DAIRCAST_VERSION=1.2.3). A build that is not
# a tag stamps 0.0.0, which every real release compares newer than.
set(AIRCAST_VERSION "0.0.0" CACHE STRING "version compiled into the binary")

# The relay this build talks to unless --signal says otherwise. Compiled in
# rather than read from a config file, for the same reason the update key is:
# a file beside the exe is one an attacker can edit, and the whole point of the
# signalling URL is that both peers agree on who is in the middle. Empty means
# "no default", and the program then insists on --signal as it always did.
set(AIRCAST_SIGNAL "wss://aircast.cloud/ws" CACHE STRING "default signalling URL")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED
    gtk4
    gstreamer-1.0
    gstreamer-sdp-1.0
    gstreamer-webrtc-1.0
    json-glib-1.0
    libsoup-3.0
    # The pairing QR. Not hand-rolled: a QR encoder is Reed-Solomon, a mask
    # score and a version table, and getting any of the three subtly wrong
    # produces a code that scans on the phone it was tested with and on no
    # other. libqrencode is in MSYS2 and in Debian, so both CI jobs have it.
    libqrencode
    # Ed25519 + BLAKE2b for the update manifest signature. Windows CNG is not an
    # alternative: bcrypt.h defines Curve25519 for ECDH only, with no EdDSA
    # algorithm identifier.
    libsodium
)

# WIN32 makes this a GUI-subsystem binary, so double-clicking it no longer
# opens a console window beside the app. mingw-w64 supplies the WinMain that
# calls main(), so nothing in main.c changes; on Linux CMake ignores the
# keyword entirely and the receiver job builds as before.
#
# The cost is that stderr has nowhere to go: a GUI-subsystem process starts
# with no console attached, so every g_printerr and every GStreamer warning is
# written to a handle that is not open. main.c answers that by opening a log
# file at startup -- without it, "run it from a terminal and send me the
# output", which is how every problem in this project has been diagnosed, stops
# working.
# The icon, as a resource rather than a file beside the exe: this is what
# Explorer, the taskbar and the Start menu shortcut read, and all three read it
# from the binary. windres ships with the MinGW toolchain that builds this.
if(WIN32)
    enable_language(RC)
    set(KAGAMI_RC "${CMAKE_CURRENT_BINARY_DIR}/kagami.rc")
    file(WRITE "${KAGAMI_RC}"
        "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/../installer/kagami.ico\"
")
endif()

add_executable(kagami WIN32 main.c update_check.c $<$<BOOL:${WIN32}>:${KAGAMI_RC}>)
target_compile_definitions(kagami PRIVATE AIRCAST_VERSION="${AIRCAST_VERSION}" AIRCAST_SIGNAL="${AIRCAST_SIGNAL}")
# style.css.h is #included by main.c and lives beside it.
target_include_directories(kagami PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${DEPS_INCLUDE_DIRS})
target_link_directories(kagami PRIVATE ${DEPS_LIBRARY_DIRS})
target_link_libraries(kagami PRIVATE ${DEPS_LIBRARIES})
if(WIN32)
    # The Wireless Display button opens ms-settings:project, a URI scheme Windows
    # implements as a COM delegate rather than a command line, so the handler
    # calls ShellExecuteW (shell32) with an apartment of its own (ole32). MinGW
    # links shell32 by default and ole32 is not; naming both is what makes that
    # not a question.
    target_link_libraries(kagami PRIVATE shell32 ole32)
endif()
target_compile_options(kagami PRIVATE ${DEPS_CFLAGS_OTHER})
