cmake_minimum_required(VERSION 3.20)
project(otsh_hash_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows + MinGW: CMake puts -I in includes_CXX.rsp; compile_commands then only
# references @...rsp, and many clangd versions do not apply those includes — IDE
# shows "file not found" while g++ builds succeed. Inlining -I fixes clangd/IDE.
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
endif()

include(FetchContent)

# --- Dependencies (header-only) ---
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "Disable non-blocking getaddrinfo on Windows" FORCE)

FetchContent_Declare(
  httplib
  # Prefer a release archive to avoid relying on `git clone` during configure.
  # (Some environments have git proxy settings that break clone.)
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.43.1.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
  nlohmann_json
  # Same reason: avoid `git clone` in constrained/proxied environments.
  URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(otsh_backend
  src/main.cpp
  src/http_server.cpp
  src/db.cpp
  src/hash.cpp
  src/ht.cpp
  src/resize.cpp
)

target_include_directories(otsh_backend PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# httplib provides target `httplib::httplib`
target_link_libraries(otsh_backend PRIVATE httplib::httplib nlohmann_json::nlohmann_json)

# Some clang/clangd setups won’t pick up transitive include dirs correctly
# unless a build directory has generated `compile_commands.json`.
# Make the header search path explicit so `<httplib.h>` resolves reliably.
target_include_directories(otsh_backend PRIVATE
  ${httplib_SOURCE_DIR}
)

# Editors should point directly at the build dir's compile_commands.json.
# Copying it to the repo root easily leaves a stale file that clangd may pick up.

# MySQL / MariaDB client library
# On Windows, ensure you have MariaDB/MySQL client installed and CMake can find the import lib.
find_path(MYSQL_INCLUDE_DIR mysql.h
  PATH_SUFFIXES include include/mysql include/mariadb
)

find_library(MYSQL_LIBRARY
  NAMES mariadb mysqlclient libmariadb libmysql
)

if (NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
  message(FATAL_ERROR "Could not find MySQL/MariaDB client headers/libs. Set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY cache vars if needed.")
endif()

target_include_directories(otsh_backend PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(otsh_backend PRIVATE ${MYSQL_LIBRARY})

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
