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

# TSan on Apple Silicon macOS is currently broken for this toolchain (see
# README "Honest limits") - this target runs it inside a disposable Linux
# container instead, which is also exactly what CI does.
tsan:
	docker run --rm -v $(CURDIR):/work -w /work ubuntu:24.04 bash -c "\
		apt-get update -qq && apt-get install -y -qq clang cmake git libhiredis-dev > /dev/null 2>&1 && \
		cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCACHE_ENABLE_TSAN=ON \
			-DCACHE_BUILD_BENCHMARKS=OFF -DCACHE_WITH_REDIS=OFF -DCMAKE_CXX_COMPILER=clang++ && \
		cmake --build build-tsan -j 4 --target unit_tests stress_test && \
		./build-tsan/unit_tests && ./build-tsan/stress_test"

redis:
	docker compose up -d redis

clean:
	rm -rf build build-tsan
