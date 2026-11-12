cmake_minimum_required(VERSION 3.20)
project(BrightDawn VERSION 0.1.0 LANGUAGES CXX)

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

# Shared compile settings. Every Bright Dawn target links this.
add_library(bd_build_flags INTERFACE)
target_compile_definitions(bd_build_flags INTERFACE BD_VERSION="${PROJECT_VERSION}")
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
target_include_directories(bd_core PUBLIC src)
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
  src/rom/nds_header.cpp
  src/rom/nitrofs.cpp
  src/rom/overlay.cpp
  src/rom/rom.cpp
)
target_link_libraries(bd_rom PUBLIC bd_core)

add_executable(bdtool tools/bdtool/main.cpp)
target_link_libraries(bdtool PRIVATE bd_rom bd_platform)

# A relocatable layout: the tool and its documents side by side.
install(TARGETS bdtool RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)

if(BD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
