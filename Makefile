.PHONY: build test bench bench-tiered tsan redis clean

build:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j 8

test: build
	./build/unit_tests
	./build/stress_test

bench: build
	./build/bench_cache --benchmark_repetitions=3 --benchmark_report_aggregates_only=true

bench-tiered: build
	./build/bench_tiered --benchmark_repetitions=5 --benchmark_report_aggregates_only=true

# Apple's bundled Xcode Command Line Tools clang (Apple Clang 17 on macOS
# 26.6, at least) crashes -fsanitize=thread on *any* program - confirmed
# down to an empty main(). Homebrew's own LLVM clang does not have this
# problem, so that's what this target uses (`brew install llvm` first).
# Falls back to a disposable Linux container - which is also exactly what
# CI's thread-sanitizer job runs - if that compiler isn't present.
LLVM_CLANGXX := /opt/homebrew/opt/llvm/bin/clang++

tsan:
ifneq ("$(wildcard $(LLVM_CLANGXX))","")
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCACHE_ENABLE_TSAN=ON \
		-DCACHE_BUILD_BENCHMARKS=OFF -DCACHE_WITH_REDIS=OFF -DCMAKE_CXX_COMPILER=$(LLVM_CLANGXX)
	cmake --build build-tsan -j 8 --target unit_tests stress_test
	./build-tsan/unit_tests
	./build-tsan/stress_test
else
	docker run --rm -v $(CURDIR):/work -w /work ubuntu:24.04 bash -c "\
		apt-get update -qq && apt-get install -y -qq clang cmake git libhiredis-dev > /dev/null 2>&1 && \
		cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCACHE_ENABLE_TSAN=ON \
			-DCACHE_BUILD_BENCHMARKS=OFF -DCACHE_WITH_REDIS=OFF -DCMAKE_CXX_COMPILER=clang++ && \
		cmake --build build-tsan -j 4 --target unit_tests stress_test && \
		./build-tsan/unit_tests && ./build-tsan/stress_test"
endif

redis:
	docker compose up -d redis

clean:
	rm -rf build build-tsan
