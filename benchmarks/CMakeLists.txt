include(FetchContent)

FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(MicroBenchmark MicroBenchmark.cpp)
target_link_libraries(MicroBenchmark PRIVATE OrderMatcher benchmark::benchmark benchmark::benchmark_main)

# Manual benchmark (no external deps)
add_executable(ManualBenchmark ManualBenchmark.cpp)
target_link_libraries(ManualBenchmark PRIVATE OrderMatcher)
