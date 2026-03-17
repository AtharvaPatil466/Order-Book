#include "OrderBook.h"
#include "Utils.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

using namespace OrderMatcher;

struct BenchStats {
  std::string name;
  std::vector<uint64_t> latencies;
  uint64_t totalTrades;
  uint64_t endBidLevels;
  uint64_t endAskLevels;

  void report() {
    if (latencies.empty())
      return;
    std::sort(latencies.begin(), latencies.end());

    auto getP = [&](double p) {
      return latencies[static_cast<size_t>(p * (latencies.size() - 1))];
    };

    uint64_t sum = std::accumulate(latencies.begin(), latencies.end(), 0ULL);
    double avg = static_cast<double>(sum) / latencies.size();

    std::cout << "\n--- " << name << " ---" << std::endl;
    std::cout << "Ops Measured:   " << latencies.size() << std::endl;
    std::cout << "Trades Executed: " << totalTrades << std::endl;
    std::cout << "Final Levels:    B=" << endBidLevels << ", A=" << endAskLevels
              << std::endl;
    std::cout << "Throughput:      " << std::fixed << std::setprecision(2)
              << (latencies.size() / (sum / 1e9)) / 1e6 << " M ops/sec"
              << std::endl;
    std::cout << "Average:         " << std::fixed << std::setprecision(1)
              << avg << " ns" << std::endl;
    std::cout << "P50 Latency:     " << getP(0.50) << " ns" << std::endl;
    std::cout << "P99 Latency:     " << getP(0.99) << " ns" << std::endl;
  }
};

inline uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::high_resolution_clock::now().time_since_epoch())
      .count();
}

void runHighRigorBenchmark() {
  std::cout << "Starting Industrial-Rigor Benchmark (Individual Timing)..."
            << std::endl;
  OrderBook book;
  std::mt19937 rng(42);
  std::uniform_int_distribution<ParticipantId> partDist(1, 1000);
  OrderId nextId = 1;

  // PRE-POPULATE (Balanced Book)
  std::cout << "Pre-populating resting liquidity..." << std::endl;
  book.resetStatus();
  book.addOrder(nextId++, 999, Side::Buy, 1000000, 1,
                OrderType::Limit); // SET REF PRICE = 1,000,000

  for (int i = 0; i < 5000; ++i) {
    // Resting: Bids @ 980k-990k, Asks @ 1010k-1020k
    book.addOrder(nextId++, partDist(rng), Side::Buy, 980000 + (i % 10000), 100,
                  OrderType::Limit);
    book.addOrder(nextId++, partDist(rng), Side::Sell, 1020000 - (i % 10000),
                  100, OrderType::Limit);
  }

  std::vector<OrderId> activeOrders;
  activeOrders.reserve(2000000);

  // BENCHMARK 1: ADDS
  size_t numAddOps = 50000;
  BenchStats addStats{"Pure Adds (Limit)", {}, 0, 0, 0};
  addStats.latencies.reserve(numAddOps);
  for (size_t i = 0; i < numAddOps; ++i) {
    OrderId id = nextId++;
    activeOrders.push_back(id);
    uint64_t t1 = now_ns();
    book.addOrder(id, partDist(rng), Side::Buy, 950000 + (i % 500), 10,
                  OrderType::Limit);
    uint64_t t2 = now_ns();
    addStats.latencies.push_back(t2 - t1);
  }
  addStats.endBidLevels = book.getBidLevelsCount();
  addStats.endAskLevels = book.getAskLevelsCount();
  addStats.report();

  // BENCHMARK 2: MATCHES (Crossing Spread but within 5% CB)
  size_t numMatchOps = 20000;
  BenchStats matchStats{"Pure Matches (Aggressive)", {}, 0, 0, 0};
  matchStats.latencies.reserve(numMatchOps);
  uint64_t tradeStart = book.getTradeCount();
  for (size_t i = 0; i < numMatchOps; ++i) {
    OrderId id = nextId++;
    Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
    // 1,040,000 and 960,000 are 4% from 1M reference.
    Price p = (s == Side::Buy) ? 1040000 : 960000;
    uint64_t t1 = now_ns();
    book.addOrder(id, partDist(rng), s, p, 10, OrderType::Limit);
    uint64_t t2 = now_ns();
    matchStats.latencies.push_back(t2 - t1);
  }
  matchStats.totalTrades = book.getTradeCount() - tradeStart;
  matchStats.endBidLevels = book.getBidLevelsCount();
  matchStats.endAskLevels = book.getAskLevelsCount();
  matchStats.report();

  // BENCHMARK 3: CANCELS
  size_t numCancelOps = 20000;
  BenchStats cancelStats{"Pure Cancels", {}, 0, 0, 0};
  cancelStats.latencies.reserve(numCancelOps);
  for (size_t i = 0; i < numCancelOps; ++i) {
    if (activeOrders.empty())
      break;
    size_t idx = rng() % activeOrders.size();
    OrderId id = activeOrders[idx];
    activeOrders[idx] = activeOrders.back();
    activeOrders.pop_back();

    uint64_t t1 = now_ns();
    book.cancelOrder(id);
    uint64_t t2 = now_ns();
    cancelStats.latencies.push_back(t2 - t1);
  }
  cancelStats.endBidLevels = book.getBidLevelsCount();
  cancelStats.endAskLevels = book.getAskLevelsCount();
  cancelStats.report();
}

int main() {
  runHighRigorBenchmark();
  return 0;
}
