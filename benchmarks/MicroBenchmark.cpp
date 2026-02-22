#include <benchmark/benchmark.h>
#include "OrderBook.h"
#include "Types.h"
#include "Utils.h"

using namespace OrderMatcher;

// Benchmark Adding Orders (No Match)
static void BM_AddOrder_NoMatch(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    // Pre-fill to avoid cold start? 
    // Or restart book every iteration? Book clearing is expensive.
    
    // We measure adding ONE order.
    // To avoid book state explosion, we might cancel it?
    // Or just fill the book.
    
    // Strategy: Add N orders.
    for (auto _ : state) {
        state.PauseTiming();
        // Prepare?
        state.ResumeTiming();
        
        book.addOrder(id++, Side::Buy, 1000000 - (id % 1000), 10, OrderType::Limit);
    }
}

// Benchmark Matching Logic
static void BM_MatchOrder(benchmark::State& state) {
    OrderBook book;
    OrderId id = 1;
    
    // Pre-seed book with Sells
    for (int i = 0; i < 10000; ++i) {
        book.addOrder(id++, Side::Sell, 100000 + i, 10, OrderType::Limit);
    }
    
    // Benchmark Buying into the Sells
    for (auto _ : state) {
        // Buy matches the lowest sell
        // We match 1 order per iteration
        book.addOrder(id++, Side::Buy, 200000, 10, OrderType::Limit);
        
        // Note: Book state changes. We consume liquidity.
        // If we run out, it becomes NoMatch.
        // For microbenchmark, we ideally reset.
        // But resetting is too slow for loop.
        // So we pre-seed enough liquidity.
    }
}

BENCHMARK(BM_AddOrder_NoMatch);
BENCHMARK(BM_MatchOrder);

BENCHMARK_MAIN();
