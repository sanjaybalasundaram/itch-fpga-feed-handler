cmake_minimum_required(VERSION 3.16)
project(itch_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic)

include_directories(include src)

add_executable(itch_bbo   src/itch_bbo.cpp)
add_executable(itch_bench src/itch_bench.cpp)
add_executable(test_book  tests/test_book.cpp)

enable_testing()
add_test(NAME unit_tests COMMAND test_book)
