# Fails if DIR holds anything: cmake -DDIR=<folder> -P expect_empty_dir.cmake
file(GLOB left "${DIR}/*")
if(left)
  message(FATAL_ERROR "${DIR} should be empty, but holds: ${left}")
endif()
