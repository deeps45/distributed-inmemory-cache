cmake_minimum_required(VERSION 3.20)
project(distributed_inmemory_cache CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(CACHE_BUILD_TESTS "Build unit and stress tests" ON)
option(CACHE_BUILD_BENCHMARKS "Build Google Benchmark suite" ON)
option(CACHE_ENABLE_TSAN "Build with -fsanitize=thread" OFF)
option(CACHE_ENABLE_ASAN "Build with -fsanitize=address,undefined" OFF)
option(CACHE_WITH_REDIS "Build the Redis L2 tier demo (requires hiredis)" ON)

add_compile_options(-Wall -Wextra)

if(CACHE_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

if(CACHE_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

# Header-only core library.
add_library(cache_core INTERFACE)
target_include_directories(cache_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cache_core INTERFACE Threads::Threads)

find_package(Threads REQUIRED)

if(CACHE_WITH_REDIS)
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
  find_library(HIREDIS_LIB hiredis)
  if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIB)
    add_library(redis_tier src/redis_client.cpp)
    target_include_directories(redis_tier PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(redis_tier PUBLIC ${HIREDIS_LIB} cache_core)
    set(CACHE_HAVE_REDIS TRUE)
  else()
    message(WARNING "hiredis not found - Redis L2 tier demo will be skipped. Install it (brew install hiredis / apt install libhiredis-dev) to enable it.")
    set(CACHE_HAVE_REDIS FALSE)
  endif()
else()
  set(CACHE_HAVE_REDIS FALSE)
endif()

if(CACHE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE cache_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)

  add_executable(stress_test tests/stress_test.cpp)
  target_link_libraries(stress_test PRIVATE cache_core)

  add_executable(quick_check tests/quick_check.cpp)
  target_link_libraries(quick_check PRIVATE cache_core)
endif()

if(CACHE_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(bench_cache benchmarks/bench_cache.cpp)
  target_link_libraries(bench_cache PRIVATE cache_core benchmark::benchmark_main)

  if(CACHE_HAVE_REDIS)
    add_executable(bench_tiered benchmarks/bench_tiered.cpp)
    target_link_libraries(bench_tiered PRIVATE redis_tier benchmark::benchmark_main)
  endif()
endif()

if(CACHE_HAVE_REDIS)
  add_executable(redis_demo src/redis_demo.cpp)
  target_link_libraries(redis_demo PRIVATE redis_tier)
endif()
