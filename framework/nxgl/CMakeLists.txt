# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxgl VERSION 0.2.0 LANGUAGES C)

include(GNUInstallDirs)

option(NXGL_BUILD_TESTS "Build host-side nxgl tests" ON)
option(NXGL_BUILD_NATIVE_TESTS
       "Build tests that link the native SDL/GL implementation" OFF)
option(NXGL_ENABLE_SANITIZERS "Build tests with Address/Undefined sanitizers" OFF)
option(NXGL_WITH_NXCOMPAT "Build the optional nxcompat reporting adapter" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_library(nxgl STATIC
  src/nxgl_diagnostics.c
  src/nxgl_logic.c
  src/nxgl_metrics.c
  src/nxgl_present.c
  src/nxgl_sdl2.c
)
add_library(nxgl::nxgl ALIAS nxgl)
set_target_properties(nxgl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(nxgl
  PUBLIC include
  PRIVATE src
)
target_link_libraries(nxgl PUBLIC PkgConfig::SDL2)
target_compile_features(nxgl PUBLIC c_std_99)
target_compile_definitions(nxgl PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(nxgl PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  -Wconversion -Wsign-conversion -Wcast-qual
)
if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nxgl PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

install(TARGETS nxgl ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxgl.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NXGL_WITH_NXCOMPAT)
  if(NOT TARGET nxcompat)
    set(NXCOMPAT_BUILD_TESTS OFF CACHE BOOL "")
    set(NXCOMPAT_BUILD_TOOLS OFF CACHE BOOL "")
    set(NXCOMPAT_WITH_SDL2 OFF CACHE BOOL "")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../nxcompat"
                     "${CMAKE_CURRENT_BINARY_DIR}/nxcompat" EXCLUDE_FROM_ALL)
  endif()
  add_library(nxgl-nxcompat STATIC src/nxgl_nxcompat.c)
  add_library(nxgl::nxcompat ALIAS nxgl-nxcompat)
  set_target_properties(nxgl-nxcompat PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxgl-nxcompat PUBLIC include)
  target_link_libraries(nxgl-nxcompat PUBLIC nxgl nxcompat)
  target_compile_features(nxgl-nxcompat PUBLIC c_std_99)
  target_compile_options(nxgl-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxgl-nxcompat PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endif()

if(NXGL_BUILD_TESTS)
  enable_testing()
  add_library(nxgl-test-environment STATIC src/nxgl_sdl2.c)
  target_include_directories(nxgl-test-environment PRIVATE
    include src ${SDL2_INCLUDE_DIRS})
  target_compile_features(nxgl-test-environment PRIVATE c_std_99)
  target_compile_definitions(nxgl-test-environment PRIVATE
    _POSIX_C_SOURCE=200809L NXGL_CORE_TESTING=1)
  target_compile_options(nxgl-test-environment PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  add_executable(test-nxgl-environment tests/test_nxgl_environment.c)
  target_include_directories(test-nxgl-environment PRIVATE
    include src ${SDL2_INCLUDE_DIRS})
  target_compile_features(test-nxgl-environment PRIVATE c_std_99)
  target_compile_definitions(test-nxgl-environment PRIVATE
    _POSIX_C_SOURCE=200809L NXGL_CORE_TESTING=1)
  target_compile_options(test-nxgl-environment PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    -Wconversion -Wsign-conversion -Wcast-qual
  )
  target_link_libraries(test-nxgl-environment PRIVATE nxgl-test-environment)
  if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(nxgl-test-environment PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(test-nxgl-environment PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(test-nxgl-environment PRIVATE
      -fsanitize=address,undefined)
  endif()
  add_test(NAME nxgl-environment COMMAND test-nxgl-environment)

  # M13 gates compile the production transactions against injected providers.
  # They intentionally consume only SDL headers: linking SDL/EGL/GLES/GL here
  # would turn a pure policy test into a possible display/device test.
  function(nxgl_configure_m13_test target)
    target_include_directories(${target} PRIVATE
      include src ${SDL2_INCLUDE_DIRS})
    target_compile_features(${target} PRIVATE c_std_99)
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L
      ${ARGN})
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endfunction()

  add_executable(test-nxgl-open-v2
    src/nxgl_logic.c
    src/nxgl_sdl2.c
    tests/test_nxgl_open_v2.c
  )
  nxgl_configure_m13_test(test-nxgl-open-v2 NXGL_M13_TESTING=1)
  add_test(NAME nxgl-m13-open-v2 COMMAND test-nxgl-open-v2)

  add_executable(test-nxgl-present-v2
    src/nxgl_present.c
    tests/test_nxgl_present_v2.c
  )
  nxgl_configure_m13_test(test-nxgl-present-v2
    NXGL_PRESENT_V2_TESTING=1)
  add_test(NAME nxgl-m13-present-v2 COMMAND test-nxgl-present-v2)

  add_executable(test-nxgl-metrics
    src/nxgl_metrics.c
    tests/test_nxgl_metrics.c
  )
  nxgl_configure_m13_test(test-nxgl-metrics)
  add_test(NAME nxgl-m13-metrics COMMAND test-nxgl-metrics)

  add_executable(test-nxgl-diagnostics
    src/nxgl_diagnostics.c
    tests/test_nxgl_diagnostics.c
  )
  nxgl_configure_m13_test(test-nxgl-diagnostics)
  add_test(NAME nxgl-m13-diagnostics COMMAND test-nxgl-diagnostics)

  if(NXGL_BUILD_NATIVE_TESTS)
    add_executable(test-nxgl tests/test_nxgl.c)
    target_include_directories(test-nxgl PRIVATE src)
    target_link_libraries(test-nxgl PRIVATE nxgl)
    target_compile_definitions(test-nxgl PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test-nxgl PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(test-nxgl PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(test-nxgl PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME nxgl-policy COMMAND test-nxgl)
  endif()

  if(NXGL_WITH_NXCOMPAT)
    add_executable(test-nxgl-nxcompat tests/test_nxgl_nxcompat.c)
    target_link_libraries(test-nxgl-nxcompat PRIVATE nxgl-nxcompat)
    target_compile_options(test-nxgl-nxcompat PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
      -Wconversion -Wsign-conversion -Wcast-qual
    )
    if(NXGL_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(test-nxgl-nxcompat PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_libraries(test-nxgl-nxcompat PRIVATE
        -fsanitize=address,undefined)
    endif()
    add_test(NAME nxgl-nxcompat COMMAND test-nxgl-nxcompat)
  endif()
endif()

if(NXGL_WITH_NXCOMPAT)
  install(TARGETS nxgl-nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxgl_nxcompat.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
