cmake_minimum_required(VERSION 3.16)
project(anoa-browser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS
    WebEngineWidgets
    WebEngineCore
    Network
    WebSockets
    Widgets
)

set(SOURCES
    src/main.cpp
)

add_executable(anoa-browser ${SOURCES})

target_sources(anoa-browser PRIVATE
    src/config/config.cpp
    src/browser/anoa_browser.cpp
    src/http/http_server.cpp
    src/cdp/cdp_proxy.cpp
    src/cdp/cdp_extensions.cpp
    src/pdf/pdf_handler.cpp
)

target_include_directories(anoa-browser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(anoa-browser PRIVATE
    Qt6::WebEngineWidgets
    Qt6::WebEngineCore
    Qt6::Network
    Qt6::WebSockets
    Qt6::Widgets
)

# Compiler warnings
if(MSVC)
    target_compile_options(anoa-browser PRIVATE /W4)
    set_property(TARGET anoa-browser PROPERTY VS_PLATFORM_TOOLSET "v143")
else()
    target_compile_options(anoa-browser PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Release strip flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_link_options(anoa-browser PRIVATE /OPT:REF /OPT:ICF)
    elseif(UNIX AND NOT APPLE)
        target_link_options(anoa-browser PRIVATE -s)
    endif()
endif()

# Install rule
include(GNUInstallDirs)
install(TARGETS anoa-browser
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
