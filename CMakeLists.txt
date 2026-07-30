cmake_minimum_required(VERSION 3.16)
project(anoa-browser VERSION 0.3.1 LANGUAGES CXX)

# CI injects the git tag here so the tag is the single source of truth for the
# version — CMakeLists.txt never needs a manual bump for releases. The value
# above is just the default for local builds.
set(ANOA_VERSION_OVERRIDE "" CACHE STRING "Version override (CI passes the git tag)")
if(ANOA_VERSION_OVERRIDE)
    string(REGEX REPLACE "^v" "" PROJECT_VERSION "${ANOA_VERSION_OVERRIDE}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Static build ────────────────────────────────────────────────────────────
option(STATIC_BUILD "Link Qt and runtime libraries statically" OFF)

if(STATIC_BUILD)
    set(BUILD_SHARED_LIBS OFF)
    cmake_policy(SET CMP0091 NEW)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(UNIX AND NOT APPLE)
        # Static libgcc/libstdc++; glibc stays dynamic (musl toolchain needed for full static)
        add_link_options(-static-libgcc -static-libstdc++)
    endif()
    # macOS: full static is unsupported by the OS; Qt .a libs are picked up automatically
    # when CMAKE_PREFIX_PATH points to a static Qt installation.
endif()
# ────────────────────────────────────────────────────────────────────────────

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

if(APPLE)
    set_target_properties(anoa-browser PROPERTIES MACOSX_BUNDLE TRUE)
endif()

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

target_compile_definitions(anoa-browser PRIVATE
    ANOA_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(anoa-browser PRIVATE
    Qt6::WebEngineWidgets
    Qt6::WebEngineCore
    Qt6::Network
    Qt6::WebSockets
    Qt6::Widgets
)

# ── Static plugin imports ───────────────────────────────────────────────────
# Required when Qt is built statically; dynamic builds ignore this block.
if(STATIC_BUILD)
    if(WIN32)
        set(_qt_platform_plugins Qt6::QWindowsIntegrationPlugin)
    elseif(APPLE)
        set(_qt_platform_plugins Qt6::QCocoaIntegrationPlugin)
    else()
        set(_qt_platform_plugins Qt6::QXcbIntegrationPlugin)
    endif()

    qt_import_plugins(anoa-browser
        INCLUDE ${_qt_platform_plugins}
        EXCLUDE_BY_TYPE imageformats
        EXCLUDE_BY_TYPE sqldrivers
        EXCLUDE_BY_TYPE printsupport
    )

    if(APPLE)
        target_link_libraries(anoa-browser PRIVATE
            "-framework AppKit"
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework CoreServices"
            "-framework CoreText"
            "-framework IOKit"
            "-framework Metal"
            "-framework QuartzCore"
            "-framework Security"
            "-framework SystemConfiguration"
        )
    elseif(UNIX)
        target_link_libraries(anoa-browser PRIVATE
            pthread dl m resolv
        )
    endif()
endif()
# ────────────────────────────────────────────────────────────────────────────

# Compiler warnings
if(MSVC)
    target_compile_options(anoa-browser PRIVATE /W4)
    set_property(TARGET anoa-browser PROPERTY VS_PLATFORM_TOOLSET "v143")
else()
    target_compile_options(anoa-browser PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── anoa-term: terminal viewer client (POSIX only, no Qt dependency) ────────
if(NOT WIN32)
    add_executable(anoa-term tools/anoa-term/anoa_term.cpp)
    target_compile_options(anoa-term PRIVATE -Wall -Wextra -Wpedantic)
endif()

# RPATH for portable Linux distribution (binary finds ./lib at runtime)
if(UNIX AND NOT APPLE)
    set_target_properties(anoa-browser PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_RPATH "$ORIGIN/lib"
        INSTALL_RPATH_USE_LINK_RPATH FALSE
    )
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
if(APPLE)
    install(TARGETS anoa-browser
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    install(TARGETS anoa-browser
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(NOT WIN32)
    install(TARGETS anoa-term
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(FILES resources/anoa-browser.desktop
    DESTINATION share/applications
)
install(FILES resources/anoa-browser.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

# Tests
option(BUILD_TESTS "Build unit and integration test targets" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()