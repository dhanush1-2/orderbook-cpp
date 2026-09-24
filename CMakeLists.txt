cmake_minimum_required(VERSION 3.25)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_BUILD_TESTS        "Build tests"                            ON)
option(OB_BUILD_BENCH        "Build benchmarks"                       OFF)
option(OB_BUILD_FUZZ         "Build libFuzzer targets"                OFF)
option(OB_BUILD_TOOLS        "Build replay and TUI tools"             OFF)
option(OB_SANITIZE           "Enable ASan + UBSan"                    OFF)
option(OB_ENABLE_INVARIANTS  "Assert book invariants after every op"   OFF)
option(OB_WARNINGS_AS_ERRORS "Treat warnings as errors"               OFF)

# Warning set. Applied only to our own targets, never to dependencies.
add_library(ob_warnings INTERFACE)
target_compile_options(ob_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wcast-align -Wunused -Wnon-virtual-dtor
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
if(OB_WARNINGS_AS_ERRORS)
    target_compile_options(ob_warnings INTERFACE -Werror)
endif()

add_library(ob_sanitize INTERFACE)
if(OB_SANITIZE)
    target_compile_options(ob_sanitize INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    target_link_options(ob_sanitize INTERFACE -fsanitize=address,undefined)
endif()

# The library is header-only: the hot path depends on cross-module inlining and
# LTO is not guaranteed across every compiler in the CI matrix.
add_library(ob INTERFACE)
target_include_directories(ob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ob INTERFACE ob_warnings ob_sanitize)
if(OB_ENABLE_INVARIANTS)
    target_compile_definitions(ob INTERFACE OB_ENABLE_INVARIANTS=1)
endif()

if(OB_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(OB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
