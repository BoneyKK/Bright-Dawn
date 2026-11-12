# The guest test corpus: C functions built for the host (the reference) and,
# where a cross-compiler is available, as an ARMv5TE image in ARM and Thumb
# state. Executors of DS code (translated code, an interpreter) are checked
# against it. Nothing here comes from the game.
enable_language(C)

# --- Host reference, on every platform ---
# The corpus is built twice under the two name prefixes the ARM image uses.
add_library(bd_guest_host_a OBJECT corpus.c)
add_library(bd_guest_host_t OBJECT corpus.c)
target_compile_definitions(bd_guest_host_t PRIVATE MODE_T)

add_executable(guest_reference_test reference_test.c
  $<TARGET_OBJECTS:bd_guest_host_a> $<TARGET_OBJECTS:bd_guest_host_t>)
add_test(NAME guest_reference COMMAND guest_reference_test)

# --- ARMv5TE image, where Clang with the ARM backend and LLD are present ---
find_program(BD_ARM_CLANG NAMES clang clang-18 clang-17 clang-16)
find_program(BD_ARM_LLD NAMES ld.lld ld.lld-18 ld.lld-17 ld.lld-16)
find_program(BD_ARM_OBJCOPY NAMES llvm-objcopy llvm-objcopy-18 llvm-objcopy-17 llvm-objcopy-16)

set(BD_ARM_FLAGS --target=armv5te-none-eabi -mcpu=arm946e-s -mfloat-abi=soft -ffreestanding
  -fno-builtin -nostdlib -O2 -fno-unwind-tables -fno-asynchronous-unwind-tables)

set(BD_GUEST_IMAGE_OK FALSE)
if(BD_ARM_CLANG AND BD_ARM_LLD AND BD_ARM_OBJCOPY)
  # Having a clang is not enough: it must have been built with the ARM backend.
  execute_process(
    COMMAND ${BD_ARM_CLANG} ${BD_ARM_FLAGS} -c ${CMAKE_CURRENT_SOURCE_DIR}/table.c
            -o ${CMAKE_CURRENT_BINARY_DIR}/probe.o
    RESULT_VARIABLE probe_result OUTPUT_QUIET ERROR_QUIET)
  if(probe_result EQUAL 0)
    set(BD_GUEST_IMAGE_OK TRUE)
  endif()
endif()

if(BD_GUEST_IMAGE_OK)
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/arm)
  set(src ${CMAKE_CURRENT_SOURCE_DIR})
  set(BD_GUEST_IMAGE ${dir}/guest_image.bin CACHE INTERNAL "ARMv5TE build of the guest corpus")
  set(BD_GUEST_IMAGE_BASE 0x02000000 CACHE INTERNAL "Load address of the guest image")
  add_custom_command(
    OUTPUT ${BD_GUEST_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${dir}
    COMMAND ${BD_ARM_CLANG} ${BD_ARM_FLAGS} -marm -c ${src}/corpus.c -o ${dir}/corpus_a.o
    COMMAND ${BD_ARM_CLANG} ${BD_ARM_FLAGS} -mthumb -DMODE_T -c ${src}/corpus.c -o ${dir}/corpus_t.o
    COMMAND ${BD_ARM_CLANG} ${BD_ARM_FLAGS} -marm -O1 -c ${src}/rt.c -o ${dir}/rt.o
    COMMAND ${BD_ARM_CLANG} ${BD_ARM_FLAGS} -marm -c ${src}/table.c -o ${dir}/table.o
    COMMAND ${BD_ARM_LLD} -T ${src}/link.ld ${dir}/table.o ${dir}/corpus_a.o ${dir}/corpus_t.o
            ${dir}/rt.o -o ${dir}/guest_image.elf
    COMMAND ${BD_ARM_OBJCOPY} -O binary ${dir}/guest_image.elf ${BD_GUEST_IMAGE}
    DEPENDS corpus.c rt.c table.c tests.def link.ld
    COMMENT "Building the ARMv5TE guest test image"
    VERBATIM)
  add_custom_target(guest_image ALL DEPENDS ${BD_GUEST_IMAGE})

  add_executable(guest_image_check image_check.c)
  add_dependencies(guest_image_check guest_image)
  add_test(NAME guest_image_structure
    COMMAND guest_image_check ${BD_GUEST_IMAGE} ${BD_GUEST_IMAGE_BASE})
  message(STATUS "Guest test image: will be built with ${BD_ARM_CLANG}")
else()
  # Listed as disabled rather than left out, so its absence is visible.
  add_test(NAME guest_image_structure COMMAND ${CMAKE_COMMAND} -E echo "no ARM cross-compiler")
  set_tests_properties(guest_image_structure PROPERTIES DISABLED TRUE)
  message(STATUS "Guest test image: skipped (needs clang with the ARM backend, ld.lld, llvm-objcopy)")
endif()

# --- Executors of DS code, checked against the corpus ---
# These need the ARM image, and Python for the code translator.
find_package(Python3 3.10 COMPONENTS Interpreter)
set(BD_TRANSLATOR_DIR ${PROJECT_SOURCE_DIR}/tools/translator)
set(BD_RUNTIME_DIR ${PROJECT_SOURCE_DIR}/src/runtime)

if(Python3_FOUND)
  # The checked-in processor model must be the one the translator emits.
  add_test(NAME translator_runtime_header
    COMMAND Python3::Interpreter ${BD_TRANSLATOR_DIR}/check_runtime_header.py ${BD_RUNTIME_DIR}/ds_cpu.h)
endif()

if(BD_GUEST_IMAGE_OK AND Python3_FOUND)
  add_library(bd_guest_runner STATIC guest_runner.cpp
    $<TARGET_OBJECTS:bd_guest_host_a> $<TARGET_OBJECTS:bd_guest_host_t>)
  target_include_directories(bd_guest_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(bd_guest_runner PUBLIC bd_build_flags)

  # Translate the image. The translator's own sources are dependencies, so a
  # change to it re-runs the translation.
  set(gen ${CMAKE_CURRENT_BINARY_DIR}/translated)
  add_custom_command(
    OUTPUT ${gen}/code.cpp ${gen}/table.cpp ${gen}/functions.h ${gen}/report.txt
    COMMAND Python3::Interpreter -B ${BD_TRANSLATOR_DIR}/translate_image.py
            ${BD_GUEST_IMAGE} ${BD_GUEST_IMAGE_BASE} ${gen}
    DEPENDS ${BD_GUEST_IMAGE} ${BD_TRANSLATOR_DIR}/translate_image.py
            ${BD_TRANSLATOR_DIR}/dslab/arm_decode.py ${BD_TRANSLATOR_DIR}/dslab/arm_analysis.py
            ${BD_TRANSLATOR_DIR}/dslab/arm_lift.py
    COMMENT "Translating the guest test image to C++"
    VERBATIM)
  add_library(bd_guest_translated STATIC ${gen}/code.cpp ${gen}/table.cpp)
  target_include_directories(bd_guest_translated PUBLIC ${gen} ${BD_RUNTIME_DIR})
  # Machine-generated code: one statement per instruction, with labels and
  # temporaries that are not always used. Warnings about it are not useful.
  if(MSVC)
    target_compile_options(bd_guest_translated PRIVATE /w)
  else()
    target_compile_options(bd_guest_translated PRIVATE -w)
  endif()

  add_executable(guest_translated_test translated_test.cpp)
  target_link_libraries(guest_translated_test PRIVATE bd_guest_runner bd_guest_translated)
  # The translator alone leaves two kinds of gap, each exercised by one test:
  # functions reached only through pointers, and unrecognised jump tables.
  add_test(NAME guest_translated
    COMMAND guest_translated_test ${BD_GUEST_IMAGE} indirect sw)
else()
  add_test(NAME guest_translated COMMAND ${CMAKE_COMMAND} -E echo "needs the ARM image and Python")
  set_tests_properties(guest_translated PROPERTIES DISABLED TRUE)
endif()
