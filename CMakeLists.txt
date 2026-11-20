cmake_minimum_required(VERSION 3.24)
project(strata LANGUAGES CXX)

option(STRATA_ASAN "Enable AddressSanitizer" OFF)
option(STRATA_TSAN "Enable ThreadSanitizer" OFF)
if(STRATA_ASAN AND STRATA_TSAN)
  message(FATAL_ERROR "Select at most one sanitizer")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
add_library(strata STATIC src/crc32c.cc src/db.cc src/wal.cc)
target_include_directories(strata PUBLIC include)
target_link_libraries(strata PRIVATE Threads::Threads)
target_compile_options(strata PRIVATE -Wall -Wextra -Werror)
if(STRATA_ASAN)
  target_compile_options(strata PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(strata PUBLIC -fsanitize=address)
elseif(STRATA_TSAN)
  target_compile_options(strata PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(strata PUBLIC -fsanitize=thread)
endif()

add_executable(strata_cli tools/strata_cli.cc)
target_link_libraries(strata_cli PRIVATE strata)
target_compile_options(strata_cli PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
  )
  FetchContent_MakeAvailable(googletest)
  add_executable(strata_test
    tests/crc32c_test.cc
    tests/db_test.cc
    tests/memtable_test.cc
    tests/recovery_test.cc
    tests/skiplist_test.cc
    tests/wal_test.cc
  )
  target_include_directories(strata_test PRIVATE src)
  target_link_libraries(strata_test PRIVATE strata GTest::gtest_main)
  target_compile_options(strata_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME strata_test COMMAND strata_test)
  set_tests_properties(strata_test PROPERTIES TIMEOUT 180)
endif()
