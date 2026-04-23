find_package(Qt6 6.4 REQUIRED COMPONENTS Test Core)

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
    anoa-config-lib
)
set_target_properties(anoa-browser-test-config PROPERTIES AUTOMOC ON)

add_test(NAME ConfigTests COMMAND anoa-browser-test-config)
