cmake_minimum_required(VERSION 3.15)
project(OrderMatchingEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Aggressive Optimizations for HFT
if(CMAKE_BUILD_TYPE MATCHES "Release")
    message(STATUS "Enabling HFT Optimizations: -O3 -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native -mtune=native -Wall -Wextra -Werror")
    
    # Link Time Optimization (LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT result OUTPUT output)
    if(result)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO Enabled")
    else()
        message(WARNING "LTO not supported: ${output}")
    endif()
endif()

# Threading support
find_package(Threads REQUIRED)

# Include directories
include_directories(include)

# Main Engine Library
add_library(OrderMatcher 
    src/OrderBook.cpp
    src/MatchingEngine.cpp
)
target_link_libraries(OrderMatcher PUBLIC Threads::Threads)

# Main Executable
add_executable(OrderEngine src/main.cpp)
target_link_libraries(OrderEngine PRIVATE OrderMatcher)

# Testing
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarking
option(BUILD_BENCHMARKS "Build benchmarks" ON)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
