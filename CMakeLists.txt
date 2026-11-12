cmake_minimum_required(VERSION 3.20)
project(BrightDawn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BD_BUILD_TESTS "Build the unit tests" ON)
option(BD_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)
option(BD_WERROR "Treat compiler warnings as errors" OFF)
option(BD_STATIC_RUNTIME "Link the C/C++ runtime statically, for release packages" OFF)

# Release packages should run without installing a compiler runtime. Not
# compatible with BD_SANITIZE. No effect on macOS, which has no static libc.
if(BD_STATIC_RUNTIME)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-static)
  endif()
endif()

# Every build identifies itself, so that output pasted into a bug report can
# be matched to the build that produced it. Release builds set BD_BUILD_ID to
# the label of their release tag.
set(BD_BUILD_ID "dev" CACHE STRING "Build label appended to the version, e.g. pre.12")
set(BD_COMMIT "" CACHE STRING "Short hash of the source revision being built")
set(BD_VERSION_STRING "${PROJECT_VERSION}-${BD_BUILD_ID}")
if(BD_COMMIT)
  string(APPEND BD_VERSION_STRING " (${BD_COMMIT})")
endif()
configure_file(src/core/version.h.in ${CMAKE_BINARY_DIR}/generated/core/version.h @ONLY)

# Shared compile settings. Every Bright Dawn target links this.
add_library(bd_build_flags INTERFACE)
if(MSVC)
  target_compile_options(bd_build_flags INTERFACE /W4 /permissive- /utf-8)
  if(BD_WERROR)
    target_compile_options(bd_build_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(bd_build_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
  if(BD_WERROR)
    target_compile_options(bd_build_flags INTERFACE -Werror)
  endif()
  if(BD_SANITIZE)
    target_compile_options(bd_build_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(bd_build_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# Module dependency rule: core <- platform, core <- rom. Nothing in core, platform
# or rom may depend on rendering, audio or input (see docs/ARCHITECTURE.md).

add_library(bd_core STATIC
  src/core/checksum.cpp
  src/core/result.cpp
)
target_include_directories(bd_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(bd_core PUBLIC bd_build_flags)

add_library(bd_platform STATIC
  src/platform/paths.cpp
)
target_link_libraries(bd_platform PUBLIC bd_core)

add_library(bd_rom STATIC
  src/rom/arm9.cpp
  src/rom/compression.cpp
  src/rom/export_path.cpp
  src/rom/file_format.cpp
  src/rom/game_id.cpp
  src/rom/narc.cpp
  src/rom/nds_header.cpp
  src/rom/nitrofs.cpp
  src/rom/overlay.cpp
  src/rom/rom.cpp
)
target_link_libraries(bd_rom PUBLIC bd_core)

# The fallback interpreter. This target wraps third-party code that is
# compiled as it comes: without this project's warning flags, and without the
# sanitizers, whose findings in it would not be ours to act on.
add_library(bd_arm_core STATIC src/runtime/interp/arm_core_skyemu.c)
target_include_directories(bd_arm_core PUBLIC src/runtime/interp PRIVATE third_party/skyemu)
set_target_properties(bd_arm_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
# Always optimised: an interpreter built without optimisation is several
# times slower, which shows directly in the test times.
if(MSVC)
  target_compile_options(bd_arm_core PRIVATE /w /O2)
  string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
else()
  target_compile_options(bd_arm_core PRIVATE -w -O2)
endif()

add_executable(bdtool tools/bdtool/main.cpp)
target_link_libraries(bdtool PRIVATE bd_rom bd_platform)

# A relocatable layout: the tool and its documents side by side.
install(TARGETS bdtool RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)

if(BD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
