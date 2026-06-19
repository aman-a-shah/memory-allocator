include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(benchmark)

add_executable(memalloc_bench
    alloc_bench.cpp)

target_link_libraries(memalloc_bench PRIVATE
    memalloc
    memalloc_warnings
    benchmark::benchmark)

# Standalone metrics generator: replays the HFT trace and writes the research
# note's data.json (throughput, latency tails, fragmentation, scalability).
add_executable(memalloc_metrics
    metrics.cpp)

target_link_libraries(memalloc_metrics PRIVATE
    memalloc
    memalloc_warnings)
