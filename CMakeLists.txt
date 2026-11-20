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

add_library(strata STATIC src/strata.cc)
target_compile_options(strata PRIVATE -Wall -Wextra -Werror)
if(STRATA_ASAN)
  target_compile_options(strata PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(strata PUBLIC -fsanitize=address)
elseif(STRATA_TSAN)
  target_compile_options(strata PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(strata PUBLIC -fsanitize=thread)
endif()

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
  )
  FetchContent_MakeAvailable(googletest)
  add_executable(strata_smoke_test tests/smoke_test.cc)
  target_link_libraries(strata_smoke_test PRIVATE strata GTest::gtest_main)
  target_compile_options(strata_smoke_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME strata_smoke_test COMMAND strata_smoke_test)
endif()
