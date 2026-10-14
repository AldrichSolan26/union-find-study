cmake_minimum_required(VERSION 3.16)
project(union_find_study CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Benchmarks are meaningless in Debug. Single-config generators (Makefiles,
# Ninja) default to Release here. Multi-config generators (Visual Studio,
# Xcode) ignore CMAKE_BUILD_TYPE: build those with --config Release.
get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(uf_tests tests/test_union_find.cpp)
target_include_directories(uf_tests PRIVATE include bench)

add_executable(uf_bench bench/bench.cpp)
target_include_directories(uf_bench PRIVATE include bench)

# The validity experiment: the same harness with the volatile sink removed.
add_executable(uf_bench_nosink bench/bench.cpp)
target_include_directories(uf_bench_nosink PRIVATE include bench)
target_compile_definitions(uf_bench_nosink PRIVATE UF_BENCH_NO_SINK)

# Side experiments behind H5, H8 and H9 (see experiments/README.md).
foreach(exp replay_vs_fresh throttling rank_branch)
  add_executable(${exp} experiments/${exp}.cpp)
  target_include_directories(${exp} PRIVATE include bench)
endforeach()

enable_testing()
add_test(NAME union_find_tests COMMAND uf_tests)
