cmake_minimum_required(VERSION 3.16)

project(DMRradio VERSION 1.0.3 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia)

# Collect OpenDMR encoder sources (OP25-based AMBE+2 encoder)
file(GLOB OPENDMR_ENC_SOURCES
    opendmr_lib/encoder/*.cc
    opendmr_lib/encoder/*.cpp
)

qt_add_executable(DMRradio
    main.cpp
    Hotspot.h          Hotspot.cpp
    HotspotManager.h   HotspotManager.cpp
    AudioEngine.h      AudioEngine.cpp
    AudioLevelBar.h    AudioLevelBar.cpp
    AmbeDecoder.h      AmbeDecoder.cpp
    AmbeEncoder.h      AmbeEncoder.cpp
    ConfigManager.h    ConfigManager.cpp
    MainWindow.h       MainWindow.cpp
    # mbelib — software AMBE+2 decoder (ISC license)
    mbelib/mbelib.c
    mbelib/ecc.c
    mbelib/ambe3600x2450.c
    mbelib/ambe3600x2400.c
    # OpenDMR encoder (OP25 MBEEncoder, GPL-2.0)
    ${OPENDMR_ENC_SOURCES}
    resources.qrc
)

if(WIN32)
    target_sources(DMRradio PRIVATE app_icon.rc)
endif()

target_include_directories(DMRradio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mbelib
    ${CMAKE_CURRENT_SOURCE_DIR}/opendmr_lib/encoder
)

set_target_properties(DMRradio PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(DMRradio PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
)

# mbelib needs libm on UNIX
if(UNIX)
    target_link_libraries(DMRradio PRIVATE m)
endif()

# Copy config.json next to the executable
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.json
    ${CMAKE_CURRENT_BINARY_DIR}/config.json
    COPYONLY
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/DMRIds.dat
    ${CMAKE_CURRENT_BINARY_DIR}/DMRIds.dat
    COPYONLY
)

install(TARGETS DMRradio
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
