# Fails if PATH exists: cmake -DPATH=<path> -P expect_absent.cmake
if(EXISTS "${PATH}")
  message(FATAL_ERROR "${PATH} should not exist")
endif()
