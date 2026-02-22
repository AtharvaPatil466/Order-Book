#!/bin/bash

# Fallback build script for Order Matching Engine
# Use this if CMake is not available.

set -e

# Detect Compiler
if command -v clang++ >/dev/null 2>&1; then
    CXX="clang++"
elif command -v g++ >/dev/null 2>&1; then
    CXX="g++"
else
    echo "Error: No C++ compiler found (clang++ or g++)."
    exit 1
fi

echo "Using compiler: $CXX"

# Optimization flags (similar to CMake Release)
FLAGS="-O3 -march=native -std=c++17 -Iinclude -Wall -Wextra -pthread"

# Create output directory
mkdir -p bin

echo "Compiling OrderMatcher library..."
$CXX $FLAGS -c src/OrderBook.cpp -o bin/OrderBook.o
$CXX $FLAGS -c src/MatchingEngine.cpp -o bin/MatchingEngine.o

echo "Compiling main executable..."
$CXX $FLAGS src/main.cpp bin/OrderBook.o bin/MatchingEngine.o -o bin/OrderEngine

echo "Compiling manual tests..."
$CXX $FLAGS tests/ManualTest.cpp bin/OrderBook.o bin/MatchingEngine.o -o bin/ManualTest

echo "Compiling manual benchmarks..."
$CXX $FLAGS benchmarks/ManualBenchmark.cpp bin/OrderBook.o bin/MatchingEngine.o -o bin/ManualBenchmark

echo "Build successful!"
echo "To run Engine: ./bin/OrderEngine"
echo "To run Tests:  ./bin/ManualTest"
echo "To run Bench:  ./bin/ManualBenchmark"
