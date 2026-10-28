cmake_minimum_required(VERSION 3.24)
project(culprit VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CULPRIT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string shown by --version")
option(CULPRIT_BUILD_TESTS "Build the tests" ON)

# One executable with nothing to install: static C++ runtime on Windows, static libstdc++ on Linux.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)  # getenv is fine here
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# FTXUI for the terminal interface: an installed copy when there is one, downloaded otherwise.
include(FetchContent)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS 7 CONFIG)
FetchContent_MakeAvailable(ftxui)
find_package(Threads REQUIRED)

# --- built-in rules: rules/*.toml compiled into the program ---------------------------------------------
file(GLOB CULPRIT_RULE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rules/*.toml")
set(CULPRIT_RULES_CPP "${CMAKE_BINARY_DIR}/generated/builtin_rules.cpp")
add_custom_command(
  OUTPUT "${CULPRIT_RULES_CPP}"
  COMMAND "${CMAKE_COMMAND}" -DDIR=${CMAKE_SOURCE_DIR}/rules -DOUT=${CULPRIT_RULES_CPP}
          -P "${CMAKE_SOURCE_DIR}/cmake/embed_rules.cmake"
  DEPENDS ${CULPRIT_RULE_FILES} "${CMAKE_SOURCE_DIR}/cmake/embed_rules.cmake"
  COMMENT "Embedding rules/*.toml"
  VERBATIM)

# --- core: parsing, filtering, reading, analysis; no terminal code, so it is easy to test --------------
add_library(culprit_core STATIC
  src/core/analyzer.cpp
  src/core/filter.cpp
  src/core/json.cpp
  src/core/model.cpp
  src/core/parser.cpp
  src/core/reader.cpp
  src/core/rules.cpp
  src/core/stack.cpp
  src/core/time.cpp
  src/core/toml.cpp
  "${CULPRIT_RULES_CPP}")
target_include_directories(culprit_core PUBLIC src)
target_link_libraries(culprit_core PUBLIC Threads::Threads)

# --- the culprit command ---------------------------------------------------------------------------------
add_executable(culprit src/main.cpp src/tui/viewer.cpp src/tui/plain.cpp src/tui/report.cpp src/tui/style.cpp)
target_link_libraries(culprit PRIVATE culprit_core ftxui::screen ftxui::dom ftxui::component)
target_compile_definitions(culprit PRIVATE CULPRIT_VERSION="${CULPRIT_VERSION}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_link_options(culprit PRIVATE -static-libstdc++ -static-libgcc)
endif()
install(TARGETS culprit RUNTIME DESTINATION bin)

if(CULPRIT_BUILD_TESTS)
  enable_testing()
  add_executable(culprit_tests
    tests/main.cpp
    tests/json_test.cpp
    tests/time_test.cpp
    tests/parser_test.cpp
    tests/filter_test.cpp
    tests/stack_test.cpp
    tests/model_test.cpp
    tests/toml_test.cpp
    tests/rules_test.cpp
    tests/analyzer_test.cpp)
  target_link_libraries(culprit_tests PRIVATE culprit_core)
  target_compile_definitions(culprit_tests PRIVATE CULPRIT_TESTDATA="${CMAKE_SOURCE_DIR}/testdata")
  add_test(NAME culprit_tests COMMAND culprit_tests)
endif()
