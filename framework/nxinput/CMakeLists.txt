# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxinput VERSION 0.1.0 LANGUAGES C)

option(NXINPUT_BUILD_TESTS "Build deterministic host-side nxinput tests" ON)
option(NXINPUT_BUILD_NATIVE_TESTS
       "Build tests that enumerate the native SDL controller subsystem" OFF)
option(NXINPUT_WITH_NXCOMPAT "Build the optional nxcompat receipt bridge" OFF)

find_package(SDL2 QUIET CONFIG)
if(TARGET SDL2::SDL2)
  set(NXINPUT_SDL_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
  set(NXINPUT_SDL_TARGET SDL2::SDL2-static)
elseif(TARGET SDL2)
  set(NXINPUT_SDL_TARGET SDL2)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(NXINPUT_SDL_TARGET PkgConfig::SDL2)
endif()

add_library(nxinput STATIC
  src/nxinput.c
  src/nxinput_core.c
)
target_include_directories(nxinput
  PUBLIC include
  PRIVATE src
)
target_compile_features(nxinput PUBLIC c_std_99)
target_compile_options(nxinput PRIVATE
  -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
)
target_link_libraries(nxinput PUBLIC ${NXINPUT_SDL_TARGET} m)

if(NXINPUT_WITH_NXCOMPAT)
  if(NOT TARGET nxcompat)
    set(NXCOMPAT_BUILD_TESTS OFF CACHE BOOL "")
    set(NXCOMPAT_BUILD_TOOLS OFF CACHE BOOL "")
    set(NXCOMPAT_WITH_SDL2 OFF CACHE BOOL "")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../nxcompat"
                     "${CMAKE_CURRENT_BINARY_DIR}/nxcompat" EXCLUDE_FROM_ALL)
  endif()
  add_library(nxinput-nxcompat STATIC src/nxinput_nxcompat.c)
  add_library(nxinput::nxcompat ALIAS nxinput-nxcompat)
  set_target_properties(nxinput-nxcompat PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(nxinput-nxcompat PUBLIC include)
  target_link_libraries(nxinput-nxcompat PUBLIC nxinput nxcompat)
  target_compile_features(nxinput-nxcompat PUBLIC c_std_99)
  target_compile_options(nxinput-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

if(NXINPUT_BUILD_TESTS)
  enable_testing()
  if(NXINPUT_BUILD_NATIVE_TESTS)
    add_executable(test-nxinput tests/test_nxinput.c)
    target_include_directories(test-nxinput PRIVATE src)
    target_link_libraries(test-nxinput PRIVATE nxinput)
    target_compile_definitions(test-nxinput PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test-nxinput PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxinput-core COMMAND test-nxinput)
  endif()
  add_test(NAME nxinput-static-gate
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/static_no_device_name_fallback.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
  if(NXINPUT_WITH_NXCOMPAT)
    add_executable(test-nxinput-nxcompat tests/test_nxinput_nxcompat.c)
    target_link_libraries(test-nxinput-nxcompat PRIVATE nxinput-nxcompat)
    target_compile_options(test-nxinput-nxcompat PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxinput-nxcompat COMMAND test-nxinput-nxcompat)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS nxinput ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxinput.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(NXINPUT_WITH_NXCOMPAT)
  install(TARGETS nxinput-nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/nxinput_nxcompat.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
