cmake_minimum_required(VERSION 3.20)
project(otsh_hash_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# --- Dependencies (header-only) ---
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "Disable non-blocking getaddrinfo on Windows" FORCE)

FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.43.1
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
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
