# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.10)
project(nxcompat VERSION 0.2.0 LANGUAGES C)

include(GNUInstallDirs)

option(NXCOMPAT_BUILD_TESTS "Build host-side nxcompat tests" ON)
option(NXCOMPAT_BUILD_TOOLS "Build the nxcompat-probe diagnostic" ON)
option(NXCOMPAT_WITH_SDL2 "Build the optional SDL2 negotiation adapter" OFF)

set(NXCOMPAT_CORE_SOURCES
  src/nxcompat.c
  src/nxcompat_backend.c
  src/nxcompat_graphics.c
  src/nxcompat_probe.c
  src/nxcompat_plan.c
  src/nxcompat_receipts.c
  src/nxcompat_registry.c
  src/nxcompat_report.c
)

function(nxcompat_configure_core target_name)
  target_include_directories(${target_name}
    PUBLIC include
    PRIVATE src
  )
  target_compile_features(${target_name} PUBLIC c_std_99)
  target_compile_definitions(${target_name} PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(${target_name} PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endfunction()

add_library(nxcompat STATIC ${NXCOMPAT_CORE_SOURCES})
add_library(nxcompat::nxcompat ALIAS nxcompat)
set_target_properties(nxcompat PROPERTIES POSITION_INDEPENDENT_CODE ON)
nxcompat_configure_core(nxcompat)

if(NXCOMPAT_WITH_SDL2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  add_library(nxcompat-sdl2 STATIC adapters/sdl2/nxcompat_sdl2.c)
  add_library(nxcompat::sdl2 ALIAS nxcompat-sdl2)
  target_include_directories(nxcompat-sdl2 PUBLIC adapters/sdl2)
  target_link_libraries(nxcompat-sdl2 PUBLIC nxcompat PkgConfig::SDL2)
  target_compile_features(nxcompat-sdl2 PUBLIC c_std_99)
  target_compile_definitions(nxcompat-sdl2 PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_options(nxcompat-sdl2 PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

if(NXCOMPAT_BUILD_TOOLS)
  add_executable(nxcompat-probe tools/nxcompat_probe_main.c)
  target_link_libraries(nxcompat-probe PRIVATE nxcompat)
  target_compile_options(nxcompat-probe PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
endif()

if(NXCOMPAT_BUILD_TESTS)
  enable_testing()

  # Fault injection is compiled into a private duplicate only.  The installed
  # nxcompat target remains byte-free of every nxcompat_test_* hook even in a
  # Debug build.
  add_library(nxcompat-test-core STATIC ${NXCOMPAT_CORE_SOURCES})
  nxcompat_configure_core(nxcompat-test-core)
  target_compile_definitions(nxcompat-test-core PRIVATE
    NXCOMPAT_CORE_TESTING=1
  )

  add_executable(test-nxcompat tests/test_nxcompat.c)
  target_link_libraries(test-nxcompat PRIVATE nxcompat-test-core)
  target_include_directories(test-nxcompat PRIVATE src)
  target_compile_definitions(test-nxcompat PRIVATE
    _POSIX_C_SOURCE=200809L
    NXCOMPAT_CORE_TESTING=1
  )
  target_compile_options(test-nxcompat PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-core COMMAND test-nxcompat)
  add_executable(test-nxcompat-m12-probe tests/test_m12_probe.c)
  target_link_libraries(test-nxcompat-m12-probe PRIVATE nxcompat)
  target_include_directories(test-nxcompat-m12-probe PRIVATE src)
  target_compile_definitions(test-nxcompat-m12-probe PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-nxcompat-m12-probe PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-m12-probe COMMAND test-nxcompat-m12-probe)
  add_executable(test-nxcompat-registry tests/test_nxcompat_registry.c)
  target_link_libraries(test-nxcompat-registry PRIVATE nxcompat)
  target_include_directories(test-nxcompat-registry PRIVATE src)
  target_compile_definitions(test-nxcompat-registry PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  target_compile_options(test-nxcompat-registry PRIVATE
    -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
  )
  add_test(NAME nxcompat-registry-receipts COMMAND test-nxcompat-registry)
  if(NXCOMPAT_WITH_SDL2)
    add_library(nxcompat-sdl2-testing STATIC
      adapters/sdl2/nxcompat_sdl2.c
    )
    target_include_directories(nxcompat-sdl2-testing PUBLIC adapters/sdl2)
    target_link_libraries(nxcompat-sdl2-testing PUBLIC
      nxcompat PkgConfig::SDL2
    )
    target_compile_features(nxcompat-sdl2-testing PUBLIC c_std_99)
    target_compile_definitions(nxcompat-sdl2-testing PRIVATE
      _POSIX_C_SOURCE=200809L
      NXCOMPAT_SDL2_TESTING=1
    )
    target_compile_options(nxcompat-sdl2-testing PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_executable(test-nxcompat-sdl2 tests/test_sdl2_adapter.c)
    target_link_libraries(test-nxcompat-sdl2 PRIVATE nxcompat-sdl2-testing)
    target_compile_definitions(test-nxcompat-sdl2 PRIVATE
      _POSIX_C_SOURCE=200809L
      NXCOMPAT_SDL2_TESTING=1
    )
    target_compile_options(test-nxcompat-sdl2 PRIVATE
      -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
    )
    add_test(NAME nxcompat-sdl2-ownership COMMAND test-nxcompat-sdl2)
  endif()
endif()

install(TARGETS nxcompat ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/nxcompat.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES capabilities-v1.json DESTINATION
        ${CMAKE_INSTALL_DATADIR}/nxcompat)
if(NXCOMPAT_BUILD_TOOLS)
  install(TARGETS nxcompat-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NXCOMPAT_WITH_SDL2)
  install(TARGETS nxcompat-sdl2 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES adapters/sdl2/nxcompat_sdl2.h
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
