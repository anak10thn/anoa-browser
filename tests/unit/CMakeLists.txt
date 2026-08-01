# Gui is for the test binary only: it verifies that QImage decodes PNG under a
# plain QCoreApplication, the assumption terminal mode is built on. It is
# deliberately NOT linked into anoa-config-lib.
find_package(Qt6 6.4 REQUIRED COMPONENTS Test Core Gui)

# Optional ASan/UBSan build of the unit targets. The parsers in frame_bytes.cpp
# read length-prefixed bytes off a socket and size a copy from numbers the peer
# chose, so an out-of-bounds read and an unsigned wrap are exactly the failures
# worth making loud rather than silent. CI runs one Linux job with this on.
option(ANOA_TEST_SANITIZE "Build the unit test targets with ASan + UBSan" OFF)

# Every target below is warning-clean under the same flags as the main binary.
function(anoa_test_target_flags target)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(ANOA_TEST_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# ── config ─────────────────────────────────────────────────────────────────
# Static library so the test binary can link against config logic without
# bringing in the full browser stack (WebEngine, Widgets, etc.).
add_library(anoa-config-lib STATIC
    ${CMAKE_SOURCE_DIR}/src/config/config.cpp
)
target_include_directories(anoa-config-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-config-lib PUBLIC
    Qt6::Core
)
set_target_properties(anoa-config-lib PROPERTIES AUTOMOC ON)

add_executable(anoa-browser-test-config test_config.cpp)
target_include_directories(anoa-browser-test-config PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(anoa-browser-test-config PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    anoa-config-lib
)
set_target_properties(anoa-browser-test-config PROPERTIES AUTOMOC ON)

add_test(NAME ConfigTests COMMAND anoa-browser-test-config)

# ── terminal ───────────────────────────────────────────────────────────────
# Both libraries below are separate targets on purpose: anoa-config-lib is
# config.cpp alone against Qt6::Core, and terminal sources must never join it
# (see AGENTS.md). They are also POSIX-only, like the terminal sources in the
# top-level CMakeLists.txt.
if(NOT WIN32)
    # The wire-facing byte parsers. Qt6::Core alone — no Gui, no event loop,
    # nothing that needs a display, which is what makes the sanitizer build
    # above cheap enough to run on every push.
    add_library(anoa-terminal-bytes-lib STATIC
        ${CMAKE_SOURCE_DIR}/src/terminal/frame_bytes.cpp
    )
    target_include_directories(anoa-terminal-bytes-lib PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(anoa-terminal-bytes-lib PUBLIC Qt6::Core)
    anoa_test_target_flags(anoa-terminal-bytes-lib)

    add_executable(anoa-browser-test-frame-bytes test_frame_bytes.cpp)
    target_link_libraries(anoa-browser-test-frame-bytes PRIVATE
        Qt6::Test
        Qt6::Core
        anoa-terminal-bytes-lib
    )
    set_target_properties(anoa-browser-test-frame-bytes PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-browser-test-frame-bytes)

    add_test(NAME FrameBytesTests COMMAND anoa-browser-test-frame-bytes)

    # The viewer UI: the input parser, the cell map and the two image-protocol
    # renderers. frame_backend.h and terminal_ui.h are listed as sources
    # because AUTOMOC only generates a metaobject for a header the target
    # actually names — the same reason the top-level CMakeLists.txt lists them.
    add_library(anoa-terminal-ui-lib STATIC
        ${CMAKE_SOURCE_DIR}/src/terminal/terminal_ui.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal/terminal_ui.h
        ${CMAKE_SOURCE_DIR}/src/terminal/frame_backend.h
    )
    target_include_directories(anoa-terminal-ui-lib PUBLIC
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(anoa-terminal-ui-lib PUBLIC Qt6::Core)
    set_target_properties(anoa-terminal-ui-lib PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-terminal-ui-lib)

    add_executable(anoa-browser-test-terminal-ui test_terminal_ui.cpp)
    target_link_libraries(anoa-browser-test-terminal-ui PRIVATE
        Qt6::Test
        Qt6::Core
        anoa-terminal-ui-lib
    )
    set_target_properties(anoa-browser-test-terminal-ui PROPERTIES AUTOMOC ON)
    anoa_test_target_flags(anoa-browser-test-terminal-ui)

    add_test(NAME TerminalUiTests COMMAND anoa-browser-test-terminal-ui)
endif()
