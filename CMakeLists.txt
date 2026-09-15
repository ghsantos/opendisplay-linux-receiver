cmake_minimum_required(VERSION 3.24)

project(opendisplay-receiver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# Ubuntu 24.04 (noble) ships Qt 6.4.2 — keep the floor there.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(PkgConfig)
find_package(Threads REQUIRED)

# Avahi provides the Bonjour advertisement; without it the receiver still
# works for senders that dial by address.
if(PkgConfig_FOUND)
    pkg_check_modules(AVAHI IMPORTED_TARGET avahi-client)
endif()

set(OPENDISPLAY_CORE_SOURCES
    include/opendisplay/advertise.hpp
    include/opendisplay/annexb.hpp
    include/opendisplay/clock_sync.hpp
    include/opendisplay/cursor_channel.hpp
    include/opendisplay/ffmpeg_decoder.hpp
    include/opendisplay/h264_sps.hpp
    include/opendisplay/log.hpp
    include/opendisplay/receiver_messages.hpp
    include/opendisplay/receiver_session.hpp
    include/opendisplay/types.hpp
    include/opendisplay/wire.hpp
    src/annexb.cpp
    src/cursor_channel.cpp
    src/ffmpeg_decoder.cpp
    src/h264_sps.cpp
    src/receiver_messages.cpp
    src/receiver_session.cpp
    src/wire.cpp
)

if(AVAHI_FOUND)
    list(APPEND OPENDISPLAY_CORE_SOURCES src/advertise.cpp)
else()
    list(APPEND OPENDISPLAY_CORE_SOURCES src/advertise_stub.cpp)
    message(WARNING
        "avahi-client not found — building without Bonjour advertising")
endif()

add_library(opendisplay-receiver-core STATIC ${OPENDISPLAY_CORE_SOURCES})
set_target_properties(opendisplay-receiver-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(opendisplay-receiver-core PUBLIC include)
target_link_libraries(opendisplay-receiver-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Threads::Threads
)
if(AVAHI_FOUND)
    target_link_libraries(opendisplay-receiver-core PUBLIC PkgConfig::AVAHI)
endif()
target_compile_options(opendisplay-receiver-core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

add_executable(opendisplay-receiver
    gui/main.cpp
    gui/receiver_window.cpp
    gui/receiver_window.hpp
    gui/video_widget.cpp
    gui/video_widget.hpp
)
target_link_libraries(opendisplay-receiver PRIVATE
    opendisplay-receiver-core
    Qt6::Widgets
)
target_compile_options(opendisplay-receiver PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

include(CTest)
if(BUILD_TESTING)
    # Pure-std tests: no Qt needed, so they also build on a bare toolchain.
    add_executable(opendisplay-wire-tests tests/wire_tests.cpp src/wire.cpp)
    target_include_directories(opendisplay-wire-tests PRIVATE include)
    target_compile_options(opendisplay-wire-tests PRIVATE -UNDEBUG)
    add_test(NAME wire COMMAND opendisplay-wire-tests)

    add_executable(opendisplay-annexb-tests tests/annexb_tests.cpp src/annexb.cpp)
    target_include_directories(opendisplay-annexb-tests PRIVATE include)
    target_compile_options(opendisplay-annexb-tests PRIVATE -UNDEBUG)
    add_test(NAME annexb COMMAND opendisplay-annexb-tests)

    add_executable(opendisplay-h264-sps-tests
        tests/h264_sps_tests.cpp src/h264_sps.cpp)
    target_include_directories(opendisplay-h264-sps-tests PRIVATE include)
    target_compile_options(opendisplay-h264-sps-tests PRIVATE -UNDEBUG)
    add_test(NAME h264-sps COMMAND opendisplay-h264-sps-tests)

    add_executable(opendisplay-clock-tests tests/clock_tests.cpp)
    target_include_directories(opendisplay-clock-tests PRIVATE include)
    target_compile_options(opendisplay-clock-tests PRIVATE -UNDEBUG)
    add_test(NAME clock COMMAND opendisplay-clock-tests)

    add_executable(opendisplay-messages-tests
        tests/messages_tests.cpp src/receiver_messages.cpp src/wire.cpp)
    target_include_directories(opendisplay-messages-tests PRIVATE include)
    target_link_libraries(opendisplay-messages-tests PRIVATE Qt6::Core)
    target_compile_options(opendisplay-messages-tests PRIVATE -UNDEBUG)
    add_test(NAME messages COMMAND opendisplay-messages-tests)
endif()

install(TARGETS opendisplay-receiver RUNTIME DESTINATION bin)
install(FILES packaging/org.opendisplay.receiver.desktop
    DESTINATION share/applications)
install(FILES resources/icon.png
    DESTINATION share/icons/hicolor/256x256/apps
    RENAME org.opendisplay.receiver.png)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "opendisplay")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libqt6widgets6, libavahi-client3, ffmpeg")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "OpenDisplay receiver: use this machine as a display for an OpenDisplay sender")
include(CPack)
