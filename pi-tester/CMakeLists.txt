cmake_minimum_required(VERSION 3.16)
project(p2_stm_modbus_tester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(p2_modbus_core
    src/modbus_rtu.cpp
)
target_include_directories(p2_modbus_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_compile_options(p2_modbus_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(p2-stm-modbus-test
    src/main.cpp
)
target_link_libraries(p2-stm-modbus-test PRIVATE p2_modbus_core)
target_compile_options(p2-stm-modbus-test PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(p2-stm-modbus-unit
    tests/test_modbus.cpp
)
target_link_libraries(p2-stm-modbus-unit PRIVATE p2_modbus_core)
target_compile_options(p2-stm-modbus-unit PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME p2-stm-modbus-unit COMMAND p2-stm-modbus-unit)
