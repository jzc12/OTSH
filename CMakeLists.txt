cmake_minimum_required(VERSION 3.20)
project(otsh_hash_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows + MinGW: CMake puts -I in includes_CXX.rsp; compile_commands then only
# references @...rsp, and many clangd versions do not apply those includes 鈥?IDE
# shows "file not found" while g++ builds succeed. Inlining -I fixes clangd/IDE.
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
endif()

include(FetchContent)

# --- Dependencies (header-only) ---
# 浼樺厛 third_party 鍗曟枃浠讹紱鍚﹀垯灏濊瘯 raw.githubusercontent.com锛堝父姣?GitHub zip 绋筹級锛?# 浠嶅け璐ュ啀鍥為€€ FetchContent zip銆傚彲鎵嬪姩鏀剧疆锛?#   third_party/cpp-httplib/httplib.h
#   third_party/nlohmann/json.hpp
set(OTSH_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(OTSH_HTTPPLIB_H "${OTSH_THIRD_PARTY}/cpp-httplib/httplib.h")
set(OTSH_JSON_HPP "${OTSH_THIRD_PARTY}/nlohmann/json.hpp")

set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "Disable non-blocking getaddrinfo on Windows" FORCE)

function(_otsh_try_download_single url dest)
  get_filename_component(_d "${dest}" DIRECTORY)
  file(MAKE_DIRECTORY "${_d}")
  file(DOWNLOAD "${url}" "${dest}" TLS_VERIFY ON STATUS _st)
  list(GET _st 0 _rc)
  if(NOT _rc EQUAL 0)
    list(GET _st 1 _msg)
    message(STATUS "  download failed (${_rc}): ${_msg}")
  endif()
endfunction()

# httplib
set(_otsh_httplib_ready FALSE)
if(EXISTS "${OTSH_HTTPPLIB_H}")
  set(_otsh_httplib_ready TRUE)
else()
  message(STATUS "Fetching httplib.h (single-file) 鈥?)
  _otsh_try_download_single(
    "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.43.1/httplib.h"
    "${OTSH_HTTPPLIB_H}")
  if(EXISTS "${OTSH_HTTPPLIB_H}")
    set(_otsh_httplib_ready TRUE)
  endif()
endif()

if(_otsh_httplib_ready)
  add_library(otsh_httplib INTERFACE)
  target_include_directories(otsh_httplib INTERFACE "${OTSH_THIRD_PARTY}/cpp-httplib")
  add_library(httplib::httplib ALIAS otsh_httplib)
  set(OTSH_HTTPPLIB_INCLUDE "${OTSH_THIRD_PARTY}/cpp-httplib")
  message(STATUS "Using httplib from ${OTSH_HTTPPLIB_INCLUDE}")
else()
  message(STATUS "Using FetchContent (zip) for httplib 鈥?)
  FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.43.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(httplib)
  set(OTSH_HTTPPLIB_INCLUDE "${httplib_SOURCE_DIR}")
endif()

# nlohmann/json
set(_otsh_json_ready FALSE)
if(EXISTS "${OTSH_JSON_HPP}")
  set(_otsh_json_ready TRUE)
else()
  message(STATUS "Fetching nlohmann/json.hpp (single-file) 鈥?)
  _otsh_try_download_single(
    "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
    "${OTSH_JSON_HPP}")
  if(EXISTS "${OTSH_JSON_HPP}")
    set(_otsh_json_ready TRUE)
  endif()
endif()

if(_otsh_json_ready)
  add_library(otsh_nlohmann_json INTERFACE)
  target_include_directories(otsh_nlohmann_json INTERFACE "${OTSH_THIRD_PARTY}")
  add_library(nlohmann_json::nlohmann_json ALIAS otsh_nlohmann_json)
  message(STATUS "Using nlohmann/json from ${OTSH_THIRD_PARTY}")
else()
  message(STATUS "Using FetchContent (zip) for nlohmann_json 鈥?)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- MySQL / MariaDB client (required for mysql storage) ---
find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES include include/mysql include/mariadb)
find_library(MYSQL_LIBRARY NAMES mariadb mysqlclient libmariadb libmysql)

add_executable(otsh_backend
  src/main.cpp
  src/http_server.cpp
  src/hash.cpp
  src/ht.cpp
  src/free_slot_tree.cpp
  src/meta_entry.cpp
  src/metrics.cpp
  src/router.cpp
  src/storage_mysql.cpp
)

target_include_directories(otsh_backend PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# httplib provides target `httplib::httplib`
target_link_libraries(otsh_backend PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
# MinGW/MSVC: Winsock锛坰ocket / WSAPoll / WSAStartup 绛夛級
if (WIN32)
  target_link_libraries(otsh_backend PRIVATE ws2_32)
endif()
if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
  message(FATAL_ERROR "MySQL/MariaDB client headers/libs not found. Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY.")
endif()

target_include_directories(otsh_backend PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(otsh_backend PRIVATE ${MYSQL_LIBRARY})
target_compile_definitions(otsh_backend PRIVATE OTSH_HAVE_MYSQL=1)

add_executable(otsh_tests
  src/test_runner.cpp
  src/hash.cpp
  src/ht.cpp
  src/free_slot_tree.cpp
  src/meta_entry.cpp
  src/metrics.cpp
  src/router.cpp
  src/storage_mysql.cpp
)
target_include_directories(otsh_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(otsh_tests PRIVATE ${MYSQL_LIBRARY})
target_include_directories(otsh_tests PRIVATE ${MYSQL_INCLUDE_DIR})
target_compile_definitions(otsh_tests PRIVATE OTSH_HAVE_MYSQL=1)
if (WIN32)
  target_compile_definitions(otsh_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Some clang/clangd setups will not pick up transitive include dirs correctly
# unless a build directory has generated `compile_commands.json`.
# Make the header search path explicit so `<httplib.h>` resolves reliably.

# OTSH perf benchmark (no MySQL)
add_executable(otsh_perf
  perf/bench.cpp
  src/hash.cpp
  src/ht.cpp
  src/free_slot_tree.cpp
  src/meta_entry.cpp
  src/metrics.cpp
  src/router.cpp
)
target_include_directories(otsh_perf PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if (WIN32)
  target_compile_definitions(otsh_perf PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

target_include_directories(otsh_backend PRIVATE ${OTSH_HTTPPLIB_INCLUDE})

# Editors should point directly at the build dir's compile_commands.json.
# Copying it to the repo root easily leaves a stale file that clangd may pick up.

if (WIN32)
  target_compile_definitions(otsh_backend PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Extra flags for clangd / clang-based editors: standalone headers need the same
# libstdc++ include paths that g++ uses. Written next to compile_commands.json.
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -dumpfullversion
    OUTPUT_VARIABLE _otsh_gcc_ver
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
    OUTPUT_VARIABLE _otsh_gcc_machine
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  get_filename_component(_otsh_gcc_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_otsh_gcc_prefix "${_otsh_gcc_bindir}/.." ABSOLUTE)
  string(REPLACE "\\" "/" _otsh_gcc_prefix "${_otsh_gcc_prefix}")
  set(_otsh_cf_lines "")
  foreach(_dir IN ITEMS
      "${_otsh_gcc_prefix}/include/c++/${_otsh_gcc_ver}"
      "${_otsh_gcc_prefix}/include/c++/${_otsh_gcc_ver}/${_otsh_gcc_machine}"
      "${_otsh_gcc_prefix}/include/c++/${_otsh_gcc_ver}/backward"
      "${_otsh_gcc_prefix}/lib/gcc/${_otsh_gcc_machine}/${_otsh_gcc_ver}/include"
      "${_otsh_gcc_prefix}/include"
      "${_otsh_gcc_prefix}/lib/gcc/${_otsh_gcc_machine}/${_otsh_gcc_ver}/include-fixed")
    string(APPEND _otsh_cf_lines "-isystem\n${_dir}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/compile_flags.txt" "${_otsh_cf_lines}")
endif()
