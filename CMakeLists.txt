cmake_minimum_required(VERSION 3.20)
project(BrightDawn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BD_BUILD_TESTS "Build the unit tests" ON)
option(BD_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)
option(BD_WERROR "Treat compiler warnings as errors" OFF)
option(BD_BUILD_APP "Build the game window, bright-dawn (needs SDL 3)" ON)
option(BD_SYSTEM_SDL "Use an installed SDL 3 instead of building the pinned release" OFF)
option(BD_STATIC_RUNTIME "Link the C/C++ runtime statically, for release packages" OFF)

# Release packages should run without installing a compiler runtime. Not
# compatible with BD_SANITIZE. No effect on macOS, which has no static libc.
if(BD_STATIC_RUNTIME)
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Fully static, except for programs that set BD_SYSTEM_LIBC: the window,
    # which loads the system's display, sound and controller libraries while
    # it runs, and so must use the system's C library. Its C++ runtime is
    # still linked in. Shared libraries are loaded into such a program, so
    # they are linked the same way.
    set(bd_system_libc
        "$<OR:$<BOOL:$<TARGET_PROPERTY:BD_SYSTEM_LIBC>>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>>")
    add_link_options("$<IF:${bd_system_libc},-static-libstdc++,-static>" "$<${bd_system_libc}:-static-libgcc>")
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
  src/core/log.cpp
  src/core/result.cpp
  src/core/sha256.cpp
  src/core/workers.cpp
)
find_package(Threads REQUIRED)
target_include_directories(bd_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(bd_core PUBLIC bd_build_flags Threads::Threads)

add_library(bd_platform STATIC
  src/platform/crash_log.cpp
  src/platform/frame_clock.cpp
  src/platform/paths.cpp
  src/platform/settings.cpp
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
  src/rom/program_code.cpp
  src/rom/rom.cpp
)
target_link_libraries(bd_rom PUBLIC bd_core)

# Decodes the game's own formats, from the player's ROM, into what the
# program can show.
add_library(bd_assets STATIC src/assets/font.cpp src/assets/screen_picture.cpp)
target_link_libraries(bd_assets PUBLIC bd_rom)

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

# Runs translated code with the interpreter behind it. Built against the
# processor model in src/runtime/ds_cpu.h, the same header translated code uses.
add_library(bd_runtime STATIC src/runtime/machine.cpp)
target_include_directories(bd_runtime PUBLIC src/runtime)
target_link_libraries(bd_runtime PUBLIC bd_build_flags PRIVATE bd_arm_core)

# Loads the game library (src/runtime/game_library.h) built from the player's
# ROM. Kept apart from bd_runtime: loading a library needs the system's C
# library, which the fully static tools do not have.
add_library(bd_game_library_host STATIC src/runtime/game_library_host.cpp)
target_link_libraries(bd_game_library_host PUBLIC bd_runtime ${CMAKE_DL_LIBS})

# The console around the processor: memory map and hardware.
add_library(bd_nds STATIC
  src/nds/arm9_bus.cpp
  src/nds/arm9_io.cpp
  src/nds/bios.cpp
  src/nds/bios_rom.cpp
  src/nds/cartridge.cpp
  src/nds/console.cpp
  src/nds/dma.cpp
  src/nds/display.cpp
  src/nds/display_engine.cpp
  src/nds/geometry_engine.cpp
  src/nds/polygon_renderer.cpp
  src/nds/interrupts.cpp
  src/nds/ipc.cpp
  src/nds/keypad.cpp
  src/nds/math_unit.cpp
  src/nds/program.cpp
  src/nds/save_keeper.cpp
  src/nds/save_memory.cpp
  src/nds/second_processor.cpp
  src/nds/sound_driver.cpp
  src/nds/sound_hardware.cpp
  src/nds/system_control.cpp
  src/nds/timeline.cpp
  src/nds/timers.cpp
  src/nds/user_settings.cpp
  src/nds/video_memory.cpp
  src/nds/video_timing.cpp)
target_include_directories(bd_nds PUBLIC src)
target_link_libraries(bd_nds PUBLIC bd_build_flags bd_runtime bd_rom)

# Presentation rules and input mapping: what the window needs that does not
# need a window, so that it can be tested without one.
add_library(bd_gfx STATIC src/gfx/screen_image.cpp src/gfx/screen_layout.cpp)
target_link_libraries(bd_gfx PUBLIC bd_core)
add_library(bd_input STATIC src/input/bindings.cpp)
target_link_libraries(bd_input PUBLIC bd_core)
# What the window program's own screens show and keep, without the screens:
# the settings and their descriptions, and following the game builder.
add_library(bd_ui STATIC src/ui/build_progress.cpp src/ui/game_settings.cpp src/ui/problem_report.cpp)
target_link_libraries(bd_ui PUBLIC bd_platform bd_gfx bd_input bd_assets)

add_executable(bdtool tools/bdtool/main.cpp tools/bdtool/boot.cpp)
target_link_libraries(bdtool PRIVATE bd_rom bd_platform bd_nds)

# The game in a window. SDL 3 (zlib licence) is the window, input and,
# later, sound layer. Unless an installed one is asked for, a pinned release
# is downloaded, checked against its hash and built as a static library with
# the rest, so that packages need no SDL installed.
if(BD_BUILD_APP)
  if(BD_SYSTEM_SDL)
    find_package(SDL3 REQUIRED CONFIG)
    set(BD_SDL_TARGET SDL3::SDL3)
  else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz
      URL_HASH SHA256=dad488474a51a0b01d547cd2834893d6299328d2e30f479a3564088b5476bae2
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(BD_SDL_TARGET SDL3::SDL3-static)
  endif()
  # Dear ImGui (MIT licence), which draws the program's own screens: the
  # launcher, the settings book and the game builder's page. Third-party code,
  # compiled as it comes, without this project's warning flags.
  add_library(bd_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(bd_imgui PUBLIC third_party/imgui)
  target_compile_features(bd_imgui PUBLIC cxx_std_20)
  target_link_libraries(bd_imgui PUBLIC ${BD_SDL_TARGET})
  if(MSVC)
    target_compile_options(bd_imgui PRIVATE /w)
  else()
    target_compile_options(bd_imgui PRIVATE -w)
  endif()

  # The window program, but for main.cpp, which bright-dawn and
  # bright-dawn-game each compile for themselves.
  add_library(bd_app STATIC
    src/app/app.cpp
    src/app/build_tools.cpp
    src/app/child_process.cpp
    src/app/game_font.cpp
    src/app/hang_watch.cpp
    src/app/fetch.cpp
    src/app/game_screen.cpp
    src/app/gui.cpp
    src/app/launcher.cpp
    src/app/menu_skin.cpp
    src/app/menu_widgets.cpp
    src/app/settings_book.cpp
    src/app/controls_page.cpp)
  target_include_directories(bd_app PUBLIC src)
  target_link_libraries(bd_app PUBLIC bd_nds bd_gfx bd_input bd_platform bd_ui bd_imgui bd_game_library_host bd_assets
                        ${BD_SDL_TARGET})

  # The game runs on the main thread, and translated code nests native calls
  # as the game nests its own (up to runtime::Machine::kDeepestNesting deep,
  # a few hundred bytes each). Windows gives a program's main thread 1 MB of
  # stack unless the program asks for more; 16 MB leaves a wide margin.
  # Elsewhere the system decides (8 MB on Linux).
  function(bd_main_stack target)
    if(WIN32)
      if(MSVC)
        target_link_options(${target} PRIVATE /STACK:16777216)
      else()
        target_link_options(${target} PRIVATE -Wl,--stack,16777216)
      endif()
    endif()
  endfunction()

  # WIN32: on Windows a window program, which opens no console.
  add_executable(bright-dawn WIN32 src/app/main.cpp)
  target_link_libraries(bright-dawn PRIVATE bd_app)
  set_target_properties(bright-dawn PROPERTIES BD_SYSTEM_LIBC ON)
  bd_main_stack(bright-dawn)

  # The game with its code translated to C++, from the user's own ROM by
  # tools/gamebuilder/build_game.py. The translation is derived from the
  # game, so it is made on the user's machine, outside this repository, and
  # is never part of a package. BD_TRANSLATED_DIR is what translate_game.py
  # wrote: table.cpp and a folder of code_NNN.cpp files per module.
  set(BD_TRANSLATED_DIR "" CACHE PATH "Translated game code to build bright-dawn-game from")
  if(BD_TRANSLATED_DIR)
    if(NOT EXISTS "${BD_TRANSLATED_DIR}/table.cpp")
      message(FATAL_ERROR "BD_TRANSLATED_DIR: no table.cpp in ${BD_TRANSLATED_DIR}")
    endif()
    file(GLOB BD_TRANSLATED_SOURCES CONFIGURE_DEPENDS "${BD_TRANSLATED_DIR}/*/code_*.cpp")
    list(LENGTH BD_TRANSLATED_SOURCES BD_TRANSLATED_COUNT)
    message(STATUS "Translated game: ${BD_TRANSLATED_COUNT} files in ${BD_TRANSLATED_DIR}")
    # Generated code: compiled as it comes, without this project's warning
    # flags. Each file includes its module's functions.h from its own folder.
    add_library(bd_translated_game STATIC "${BD_TRANSLATED_DIR}/table.cpp" ${BD_TRANSLATED_SOURCES})
    target_include_directories(bd_translated_game PRIVATE src/runtime)
    target_compile_features(bd_translated_game PRIVATE cxx_std_20)
    if(MSVC)
      target_compile_options(bd_translated_game PRIVATE /w /bigobj)
    else()
      target_compile_options(bd_translated_game PRIVATE -w)
    endif()
    add_executable(bright-dawn-game WIN32 src/app/main.cpp)
    target_compile_definitions(bright-dawn-game PRIVATE BD_TRANSLATED_GAME=1)
    target_link_libraries(bright-dawn-game PRIVATE bd_app bd_translated_game)
    set_target_properties(bright-dawn-game PROPERTIES BD_SYSTEM_LIBC ON)
    bd_main_stack(bright-dawn-game)
  endif()
endif()

# What a player gets (cmake --install): the game's window, the licences that
# come with it, and what building the game from their ROM needs. The
# developer tools, tests and documents stay in the repository. The GPL is
# this project's; the MIT licences of SkyEmu and Dear ImGui must accompany
# copies of their code, which the program contains; SDL's zlib licence does
# not ask for it, but it is shipped too.
if(TARGET bright-dawn)
  install(TARGETS bright-dawn RUNTIME DESTINATION .)
  install(FILES third_party/imgui/LICENSE.txt DESTINATION licenses RENAME DearImGui.txt)
  if(NOT BD_SYSTEM_SDL)
    install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME SDL3.txt)
  endif()
  # The builder: the program's launcher builds the game library on the
  # player's computer with these (tools/gamebuilder/build_game.py): the
  # translator, which turns the game's code from their ROM into C++ there,
  # and the headers and glue the translated code is compiled with. The
  # program itself reads the code out of the ROM. Nothing of the game is in
  # it.
  install(FILES LICENSE DESTINATION builder)
  install(FILES src/runtime/ds_cpu.h src/runtime/translated_table.h src/runtime/game_library.h
                src/runtime/game_library_glue.cpp DESTINATION builder/src/runtime)
  install(DIRECTORY tools/gamebuilder tools/translator DESTINATION builder/tools
          PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
endif()
install(FILES LICENSE DESTINATION . RENAME LICENSE.txt)
install(FILES third_party/skyemu/LICENSE DESTINATION licenses RENAME SkyEmu.txt)

if(BD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
