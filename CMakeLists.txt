cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

# Use C++17 — required for std::optional, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build in Release mode by default (optimized, no debug overhead)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags:
#   -O2           = optimize for speed
#   -Wall -Wextra = show all warnings (good practice)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -Wall -Wextra")

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(benchmark REQUIRED)

# ── Main executable ───────────────────────────────────────────────────────────
set(SOURCES
    src/feed/itch_parser.cpp
    src/book/order_book.cpp
    src/features/feature_engine.cpp
    src/main.cpp
)

add_executable(lob_engine ${SOURCES})
target_include_directories(lob_engine PRIVATE include)

# ── Google Benchmark binary ───────────────────────────────────────────────────
add_executable(lob_bench
    bench/bench_book.cpp
    bench/bench_pipeline.cpp
    src/feed/itch_parser.cpp
    src/book/order_book.cpp
    src/features/feature_engine.cpp
)
target_include_directories(lob_bench PRIVATE include)
target_link_libraries(lob_bench PRIVATE benchmark::benchmark_main)

# ── Correctness tests ─────────────────────────────────────────────────────────
add_executable(lob_test
    test/test_order_book.cpp
    src/book/order_book.cpp
    src/feed/itch_parser.cpp
    src/features/feature_engine.cpp
)
target_include_directories(lob_test PRIVATE include)
