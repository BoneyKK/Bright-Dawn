# Test support: a synthetic ROM builder and reference encoders. No game data is
# involved; every ROM the tests use is generated here.
add_library(bd_test_support STATIC codecs.cpp rom_builder.cpp)
target_include_directories(bd_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bd_test_support PUBLIC bd_rom)

add_executable(bd_tests
  test_main.cpp
  test_checksum.cpp
  test_compression.cpp
  test_formats.cpp
  test_narc.cpp
  test_rom.cpp
)
target_link_libraries(bd_tests PRIVATE bd_test_support bd_platform)
add_test(NAME unit COMMAND bd_tests)

# End-to-end checks of the command-line tool against a generated ROM.
add_executable(make_test_rom make_test_rom.cpp)
target_link_libraries(make_test_rom PRIVATE bd_test_support)

set(BD_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/cli)
add_test(NAME cli_make_rom
  COMMAND make_test_rom ${BD_TEST_DIR}/dark_dawn_like.nds ${BD_TEST_DIR}/other_game.nds)
set_tests_properties(cli_make_rom PROPERTIES FIXTURES_SETUP test_roms)

function(bd_cli_test name)
  add_test(NAME ${name} COMMAND bdtool ${ARGN})
  set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED test_roms)
endfunction()

bd_cli_test(cli_validate validate ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_validate PROPERTIES PASS_REGULAR_EXPRESSION "OK: Golden Sun: Dark Dawn")

bd_cli_test(cli_validate_other_game validate ${BD_TEST_DIR}/other_game.nds)
set_tests_properties(cli_validate_other_game PROPERTIES
  WILL_FAIL FALSE PASS_REGULAR_EXPRESSION "not Golden Sun: Dark Dawn")

bd_cli_test(cli_validate_missing validate ${BD_TEST_DIR}/does_not_exist.nds)
set_tests_properties(cli_validate_missing PROPERTIES PASS_REGULAR_EXPRESSION "does not exist")

bd_cli_test(cli_info info ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_info PROPERTIES PASS_REGULAR_EXPRESSION "Compressed:   yes")

bd_cli_test(cli_list list ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_list PROPERTIES PASS_REGULAR_EXPRESSION "data/model/hero.nsbmd")

bd_cli_test(cli_survey survey ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_survey PROPERTIES PASS_REGULAR_EXPRESSION "LZ10 > NSBMD")

# The survey must look through 0x40 compression and into archives.
bd_cli_test(cli_survey_archives survey ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_survey_archives PROPERTIES
  PASS_REGULAR_EXPRESSION "2 archive.s. opened, 4 member.s..*LZ40 > NSBTX +1 ")

bd_cli_test(cli_lz40_probe lz40-probe ${BD_TEST_DIR}/dark_dawn_like.nds)
set_tests_properties(cli_lz40_probe PROPERTIES
  PASS_REGULAR_EXPRESSION "negated +big-endian +3 +3")

bd_cli_test(cli_extract extract ${BD_TEST_DIR}/dark_dawn_like.nds --code --out ${BD_TEST_DIR}/out)
set_tests_properties(cli_extract PROPERTIES PASS_REGULAR_EXPRESSION "Wrote [0-9]+ file")

bd_cli_test(cli_extract_missing_path extract ${BD_TEST_DIR}/dark_dawn_like.nds
  --out ${BD_TEST_DIR}/out no/such/file.bin)
set_tests_properties(cli_extract_missing_path PROPERTIES
  PASS_REGULAR_EXPRESSION "no such file in the ROM")
