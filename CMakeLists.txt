cmake_minimum_required(VERSION 3.20)
project(cpp_lob_engine LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard & options
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LOB_BUILD_TESTS "Build C++ unit/golden/fuzz tests" ON)
option(LOB_BUILD_BENCH "Build benchmark harness + Google Benchmark targets" ON)
option(LOB_BUILD_PYTHON "Build the pybind11 module (lobpy)" ON)
option(LOB_SANITIZE "Enable Address + Undefined Behaviour sanitizers" OFF)

# Strict warnings for *our* code only (not fetched third-party).
set(LOB_WARNINGS -Wall -Wextra -Wpedantic -Werror)

if(LOB_SANITIZE)
  message(STATUS "lob: ASan + UBSan enabled")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Core library: headers + a small amount of non-template impl (generator, CSV).
# ---------------------------------------------------------------------------
add_library(lob_core STATIC
  src/generator.cpp
  src/replay.cpp
)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob_core PRIVATE ${LOB_WARNINGS})
set_target_properties(lob_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Tests (GoogleTest via FetchContent)
# ---------------------------------------------------------------------------
if(LOB_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/test_ref_engine.cpp
    tests/test_generator.cpp
    tests/test_csv.cpp
    tests/test_golden_crossval.cpp
    tests/test_fuzz.cpp
  )
  target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest GTest::gtest_main)
  target_compile_options(lob_tests PRIVATE ${LOB_WARNINGS})

  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(LOB_BUILD_BENCH)
  # Custom latency/throughput harness (no external deps).
  add_executable(lob_bench_harness bench/bench_harness.cpp)
  target_link_libraries(lob_bench_harness PRIVATE lob_core)
  target_compile_options(lob_bench_harness PRIVATE ${LOB_WARNINGS})

  # Google Benchmark microbenchmarks.
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_microbench bench/bench_engines.cpp)
  target_link_libraries(lob_microbench PRIVATE lob_core benchmark::benchmark)
  target_compile_options(lob_microbench PRIVATE ${LOB_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------
if(LOB_BUILD_PYTHON)
  FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
  )
  FetchContent_MakeAvailable(pybind11)

  pybind11_add_module(lobpy python/bindings.cpp)
  target_link_libraries(lobpy PRIVATE lob_core)
  target_compile_options(lobpy PRIVATE ${LOB_WARNINGS})
endif()
