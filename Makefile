# Fallback build for machines without CMake (g++ or clang++ on Linux, macOS,
# or MSYS2/MinGW on Windows). Uses the same flags as a CMake Release build.
#
#   make            build everything into build/
#   make test       build and run the tests
#   make CXX=clang++

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
INCLUDES  = -Iinclude -Ibench
HEADERS   = include/union_find.hpp include/variants.hpp bench/workloads.hpp bench/harness.hpp

all: build/uf_tests build/uf_bench build/uf_bench_nosink

build:
	mkdir -p build

build/uf_tests: tests/test_union_find.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

build/uf_bench: bench/bench.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

build/uf_bench_nosink: bench/bench.cpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) -DUF_BENCH_NO_SINK $(INCLUDES) $< -o $@

test: build/uf_tests
	./build/uf_tests

# Side experiments behind H5, H8 and H9 (see experiments/README.md).
EXPERIMENTS = build/replay_vs_fresh build/throttling build/rank_branch
experiments: $(EXPERIMENTS)

build/%: experiments/%.cpp experiments/common.hpp $(HEADERS) | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf build

.PHONY: all test experiments clean
