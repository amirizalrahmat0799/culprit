cmake_minimum_required(VERSION 3.24)
project(culprit VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CULPRIT_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string shown by --version")
option(CULPRIT_BUILD_TESTS "Build the tests" ON)
option(CULPRIT_AI "Build the AI investigator (culprit investigate, culprit ai)" ON)
option(CULPRIT_WEB "Build the web viewer (culprit web)" ON)

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

# cpp-httplib for culprit web: one header, pinned by its hash. CULPRIT_HTTPLIB_DIR can point at a copy instead.
if(CULPRIT_WEB)
  set(CULPRIT_HTTPLIB_VERSION "0.62.0")
  set(CULPRIT_HTTPLIB_SHA256 "2d02fed52c7ddc501bbc489c770bcd7e4e92cf257a59c0b9048d59133bdaf7a0")
  set(CULPRIT_HTTPLIB_DIR "" CACHE PATH "Folder holding httplib.h ${CULPRIT_HTTPLIB_VERSION}; downloaded when empty")
  if(NOT CULPRIT_HTTPLIB_DIR)
    set(CULPRIT_HTTPLIB_DIR "${CMAKE_BINARY_DIR}/_deps/httplib")
    file(DOWNLOAD "https://raw.githubusercontent.com/yhirose/cpp-httplib/v${CULPRIT_HTTPLIB_VERSION}/httplib.h"
         "${CULPRIT_HTTPLIB_DIR}/httplib.h"
         EXPECTED_HASH SHA256=${CULPRIT_HTTPLIB_SHA256}
         TLS_VERIFY ON)
  endif()
endif()

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

# --- AI investigator: talks to Ollama or an AI API through the system's HTTP library ------------------
if(CULPRIT_AI)
  add_library(culprit_ai STATIC
    src/ai/config.cpp
    src/ai/http.cpp
    src/ai/investigator.cpp
    src/ai/json.cpp
    src/ai/provider.cpp
    src/ai/subject.cpp
    src/ai/workspace.cpp)
  target_link_libraries(culprit_ai PUBLIC culprit_core ${CMAKE_DL_LIBS})
  if(WIN32)
    target_link_libraries(culprit_ai PUBLIC winhttp)
  endif()
  target_compile_definitions(culprit_ai PUBLIC CULPRIT_WITH_AI)
endif()

# --- reports: the analysis (and an AI finding) as text, Markdown or JSON; no terminal library ---------------
add_library(culprit_output STATIC src/tui/report.cpp src/tui/style.cpp)
target_link_libraries(culprit_output PUBLIC culprit_core)
if(CULPRIT_AI)
  target_sources(culprit_output PRIVATE src/tui/finding.cpp)
  target_link_libraries(culprit_output PUBLIC culprit_ai)
endif()

# --- web viewer: a small server on 127.0.0.1 with the page compiled in -----------------------------------
if(CULPRIT_WEB)
  set(CULPRIT_WEB_PAGE_CPP "${CMAKE_BINARY_DIR}/generated/web_page.cpp")
  add_custom_command(
    OUTPUT "${CULPRIT_WEB_PAGE_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${CMAKE_SOURCE_DIR}/src/web/index.html -DOUT=${CULPRIT_WEB_PAGE_CPP}
            -DSYMBOL=kWebPage -P "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/web/index.html" "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
    COMMENT "Embedding the web viewer page"
    VERBATIM)
  add_library(culprit_web STATIC src/web/server.cpp src/web/browser.cpp "${CULPRIT_WEB_PAGE_CPP}")
  target_include_directories(culprit_web SYSTEM PRIVATE "${CULPRIT_HTTPLIB_DIR}")
  target_link_libraries(culprit_web PUBLIC culprit_output)
  # long polls each hold a worker thread while they wait: allow a few browser tabs' worth
  target_compile_definitions(culprit_web PUBLIC CULPRIT_WITH_WEB CPPHTTPLIB_THREAD_POOL_COUNT=16)
  if(WIN32)
    target_link_libraries(culprit_web PUBLIC ws2_32 shell32)
  endif()
endif()

# --- the culprit command ---------------------------------------------------------------------------------
add_executable(culprit src/main.cpp src/cli.cpp src/tui/viewer.cpp src/tui/plain.cpp)
target_link_libraries(culprit PRIVATE culprit_core culprit_output ftxui::screen ftxui::dom ftxui::component)
if(CULPRIT_AI)
  target_sources(culprit PRIVATE src/cli_ai.cpp)
  target_link_libraries(culprit PRIVATE culprit_ai)
endif()
if(CULPRIT_WEB)
  target_sources(culprit PRIVATE src/cli_web.cpp)
  target_link_libraries(culprit PRIVATE culprit_web)
endif()
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
  if(CULPRIT_AI)
    target_sources(culprit_tests PRIVATE tests/ai_json_test.cpp tests/ai_workspace_test.cpp tests/ai_investigator_test.cpp)
    target_link_libraries(culprit_tests PRIVATE culprit_ai)
  endif()
  if(CULPRIT_WEB)
    target_sources(culprit_tests PRIVATE tests/web_test.cpp)
    target_include_directories(culprit_tests SYSTEM PRIVATE "${CULPRIT_HTTPLIB_DIR}")
    target_link_libraries(culprit_tests PRIVATE culprit_web)
  endif()
  target_compile_definitions(culprit_tests PRIVATE CULPRIT_TESTDATA="${CMAKE_SOURCE_DIR}/testdata")
  add_test(NAME culprit_tests COMMAND culprit_tests)
endif()
