cmake_minimum_required(VERSION 3.20)
project(otsh_hash_backend LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
endif()

# ----- Vendored SQLite amalgamation (third_party/sqlite3) -----
add_library(otsh_sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(otsh_sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3)
target_compile_definitions(otsh_sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_DQS=0
  SQLITE_MAX_EXPR_DEPTH=0
)
if (WIN32)
  target_compile_definitions(otsh_sqlite3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
  target_compile_options(otsh_sqlite3 PRIVATE -w)  # 静音 SQLite 自身 warning
endif()
if (UNIX AND NOT APPLE)
  target_link_libraries(otsh_sqlite3 PUBLIC dl pthread)
endif()

set(OTSH_CORE_SOURCES
  src/hash.cpp
  src/ht.cpp
  src/free_slot_tree.cpp
  src/meta_entry.cpp
  src/metrics.cpp
  src/router.cpp
  src/kkick.cpp
  src/prefix_router.cpp
  src/mini_array.cpp
  src/bin_free_map.cpp
  src/system_params.cpp
  src/keystore.cpp
  src/v1_store.cpp
  src/v2_sqlite_store.cpp
  src/v3_hot_tail_store.cpp
)

add_executable(otsh_tests
  src/test_runner.cpp
  ${OTSH_CORE_SOURCES}
)
target_include_directories(otsh_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(otsh_tests PRIVATE otsh_sqlite3)
if (WIN32)
  target_compile_definitions(otsh_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

add_executable(otsh_experiment
  experiments/ch4_experiment.cpp
  ${OTSH_CORE_SOURCES}
)
target_include_directories(otsh_experiment PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(otsh_experiment PRIVATE otsh_sqlite3)
if (WIN32)
  target_compile_definitions(otsh_experiment PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(otsh_experiment PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  elseif (MSVC)
    target_compile_options(otsh_experiment PRIVATE /utf-8)
  endif()
endif()

add_executable(otsh_ch5_variants
  experiments/ch5_variants.cpp
  ${OTSH_CORE_SOURCES}
)
target_include_directories(otsh_ch5_variants PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(otsh_ch5_variants PRIVATE otsh_sqlite3)
if (WIN32)
  target_compile_definitions(otsh_ch5_variants PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(otsh_ch5_variants PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  elseif (MSVC)
    target_compile_options(otsh_ch5_variants PRIVATE /utf-8)
  endif()
endif()

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