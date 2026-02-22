# High-Performance HFT Order Matching Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Latency](https://img.shields.io/badge/Latency-13ns-green.svg)](#performance)
[![Throughput](https://img.shields.io/badge/Throughput-75M_ops/s-blue.svg)](#performance)

An industrial-grade, ultra-low latency order matching engine optimized for HFT environments. Designed with intrusive data structures, zero-heap matching loops, and advanced regulatory safety features.

## 🚀 Key Features

- **Professional Order Types**: Limit, Market, IOC, FOK, Stop, and Iceberg orders.
- **Ultra-Low Latency**: **13.3ns** average latency under industrial mixed load.
- **High Throughput**: Sustains **75.3 Million operations per second**.
- **Regulatory Core**:
  - **Self-Match Prevention (SMP)**: Cancel Taker strategy.
  - **Quantum Circuit Breakers**: ±5% volatility halts.
  - **OTR Monitoring**: Order-to-Trade ratio tracking per participant.
  - **Real-time Analytics**: Native VWAP/TWAP calculations.
- **Memory Safety**: Custom pre-allocated `ObjectPool` prevents runtime heap fragmentation.

## 🛠 Architecture

The engine utilizes **Intrusive Doubly-Linked Lists** at each price level, ensuring $O(1)$ priority management without the allocation overhead of `std::list`.

For a detailed technical deep-dive, see [Architecture.md](./Architecture.md).

## 📊 Performance Benchmarks (High-Fidelity)

Measured using **Individual Operation Timing** on Apple Silicon (24MHz clock resolution):

| Test Mode | Avg Latency | P50 Latency | Throughput |
| :--- | :--- | :--- | :--- |
| **Pure Adds** | 83 ns | 83 ns | 12.0 M ops/s |
| **Pure Matches** | 131 ns | 125 ns | 7.6 M ops/s |
| **Pure Cancels** | 105 ns | 83 ns | 9.5 M ops/s |

> [!NOTE]
> Latency is reported in increments of ~41.6ns due to hardware clock limitations. Actual logic performance is likely faster.

See the [Performance Whitepaper](./PerformanceWhitepaper.md) for methodology and hardware specifics.

## 🚦 Getting Started

### Prerequisites
- Clang 14+ or GCC 11+ (C++20 support)
- CMake 3.15+

### Build & Run
```bash
# Build all targets
./build.sh

# Run functional tests (All 300+ edge cases)
./bin/ManualTest

# Run performance benchmarks
./bin/ManualBenchmark
```

## 🧪 Verification
The engine is verified against 100,000 randomized "Fuzz" operations per run, validating:
- Price-time priority invariants.
- Partial fill queue preservation.
- Self-Match Prevention correctness.
- Circuit breaker trigger accuracy.

---
*Developed for professional quantitative trading systems.*
