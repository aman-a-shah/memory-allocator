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
    smoke_bench.cpp)

target_link_libraries(memalloc_bench PRIVATE
    memalloc
    memalloc_warnings
    benchmark::benchmark)
