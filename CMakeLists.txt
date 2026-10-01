cmake_minimum_required(VERSION 3.20)
project(kolma
        VERSION 0.1.0
        DESCRIPTION "A pluggable compression engine with an analyser, a parallel chunk engine and a terminal UI"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(KOLMA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(KOLMA_SANITIZE "Build with address+undefined sanitizers" OFF)

add_library(kolma_flags INTERFACE)
target_compile_features(kolma_flags INTERFACE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(kolma_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(KOLMA_WARNINGS_AS_ERRORS)
    target_compile_options(kolma_flags INTERFACE -Werror)
  endif()
  if(KOLMA_SANITIZE)
    target_compile_options(kolma_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kolma_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------- core library
add_library(kolma_core STATIC
  core/src/util.cpp
  core/src/bitio.cpp
  core/src/file.cpp
  core/src/algorithm.cpp
  core/src/analyzer.cpp
  core/src/archive.cpp
  core/src/engine.cpp
  core/src/algorithms/store.cpp
  core/src/algorithms/rle.cpp
  core/src/algorithms/huffman.cpp
  core/src/algorithms/lz77.cpp
  core/src/algorithms/lzw.cpp
  core/src/algorithms/bwt_mtf_ari.cpp
  core/src/algorithms/delta_rle.cpp
)
target_include_directories(kolma_core PUBLIC core/include)
target_link_libraries(kolma_core PUBLIC kolma_flags Threads::Threads)

find_package(Threads REQUIRED)

# ------------------------------------------------------------------------ CLI
add_executable(kolma cli/main.cpp)
target_link_libraries(kolma PRIVATE kolma_core)

# ---------------------------------------------------------------------- tests
enable_testing()
add_executable(kolma_tests
  tests/test_main.cpp
  tests/test_util.cpp
  tests/test_algorithms.cpp
  tests/test_archive.cpp
  tests/test_engine.cpp
  tests/test_analyzer.cpp
)
target_link_libraries(kolma_tests PRIVATE kolma_core)
add_test(NAME kolma_tests COMMAND kolma_tests)

install(TARGETS kolma RUNTIME DESTINATION bin)
