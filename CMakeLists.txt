cmake_minimum_required(VERSION 3.20)
project(kolma
        VERSION 1.1.0
        DESCRIPTION "A pluggable compression engine with an analyser, a parallel chunk engine and a terminal UI"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(WIN32)
  # windows.h otherwise defines min/max macros that break std::min/std::max.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(KOLMA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(KOLMA_SANITIZE "Build with address+undefined sanitizers" OFF)
option(KOLMA_FUZZ "Build the libFuzzer harness (Clang only)" OFF)

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
  core/src/bundle.cpp
  core/src/stdarchive.cpp
  core/src/media.cpp
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

# Standard-format archives (zip / tar / tar.gz / tar.xz / 7z) via libarchive.
# Optional: when present, kolma produces and opens universally recognised
# archives; without it the native .kolma engine still builds and works.
find_package(LibArchive QUIET)
if(LibArchive_FOUND)
  target_compile_definitions(kolma_core PUBLIC KOLMA_HAVE_LIBARCHIVE)
  target_link_libraries(kolma_core PUBLIC LibArchive::LibArchive)
  message(STATUS "kolma: libarchive ${LibArchive_VERSION} found - standard formats enabled")
else()
  message(STATUS "kolma: libarchive not found - standard formats disabled")
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------------------ CLI
add_executable(kolma cli/main.cpp)
target_link_libraries(kolma PRIVATE kolma_core)

# ------------------------------------------------------------- desktop GUI --
# The Qt6 app links the engine directly. Optional: without Qt6 the engine, CLI
# and tests still build. AUTOMOC handles the Q_OBJECT classes.
option(KOLMA_GUI "Build the Qt6 desktop app (needs Qt6 Widgets)" ON)
if(KOLMA_GUI)
  find_package(Qt6 6.2 COMPONENTS Widgets QUIET)
  if(Qt6_FOUND)
    if(APPLE)
      # macOS 14+ SDKs removed the legacy AGL framework, but prebuilt Qt's
      # exported CMake configs still list it (via WrapOpenGL and the
      # Gui/Widgets/PrintSupport link interfaces). Qt does not need AGL, so
      # strip it to avoid "ld: framework 'AGL' not found" on modern SDKs.
      foreach(_qt_tgt WrapOpenGL::WrapOpenGL Qt6::Gui Qt6::Widgets Qt6::PrintSupport)
        if(TARGET ${_qt_tgt})
          get_target_property(_qt_libs ${_qt_tgt} INTERFACE_LINK_LIBRARIES)
          if(_qt_libs)
            list(FILTER _qt_libs EXCLUDE REGEX "AGL")
            set_target_properties(${_qt_tgt} PROPERTIES INTERFACE_LINK_LIBRARIES "${_qt_libs}")
          endif()
        endif()
      endforeach()
    endif()
    add_executable(kolma-desktop
      app/main.cpp
      app/mainwindow.cpp
      app/compressview.cpp
      app/shrinkview.cpp
      app/extractview.cpp
      app/toolsview.cpp
      app/benchview.cpp
      app/settingsdialog.cpp
      app/worker.cpp
    )
    set_target_properties(kolma-desktop PROPERTIES AUTOMOC ON)
    if(APPLE)
      set_target_properties(kolma-desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "kolma"
        MACOSX_BUNDLE_GUI_IDENTIFIER "in.langersword.kolma"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      )
    endif()
    target_include_directories(kolma-desktop PRIVATE app)
    target_link_libraries(kolma-desktop PRIVATE kolma_core Qt6::Widgets)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # Qt code is not held to the engine's -Werror conversion discipline.
      target_compile_options(kolma-desktop PRIVATE -Wno-error)
    endif()
    install(TARGETS kolma-desktop RUNTIME DESTINATION bin BUNDLE DESTINATION .)
    message(STATUS "kolma-desktop: Qt6 ${Qt6_VERSION} found, building the GUI")
  else()
    message(STATUS "kolma-desktop: Qt6 not found, skipping the GUI")
  endif()
endif()

# ---------------------------------------------------------------------- tests
enable_testing()
add_executable(kolma_tests
  tests/test_main.cpp
  tests/test_util.cpp
  tests/test_algorithms.cpp
  tests/test_archive.cpp
  tests/test_auth.cpp
  tests/test_engine.cpp
  tests/test_analyzer.cpp
  tests/test_bundle.cpp
  tests/test_stdarchive.cpp
  tests/test_media.cpp
)
target_link_libraries(kolma_tests PRIVATE kolma_core)
add_test(NAME kolma_tests COMMAND kolma_tests)

# ------------------------------------------------------------------ fuzzing
if(KOLMA_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KOLMA_FUZZ requires Clang (libFuzzer)")
  endif()
  target_compile_options(kolma_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(kolma_flags INTERFACE -fsanitize=address,undefined)
  add_executable(kolma_fuzz tests/fuzz_archive.cpp)
  target_link_libraries(kolma_fuzz PRIVATE kolma_core)
  target_compile_options(kolma_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(kolma_fuzz PRIVATE -fsanitize=fuzzer)
endif()

install(TARGETS kolma RUNTIME DESTINATION bin)
