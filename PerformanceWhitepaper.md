# HFT Order Matching Engine - Performance Whitepaper (High-Fidelity)

## 1. Executive Summary
This document presents the finalized performance metrics for the C++ HFT Order Matching Engine. Following a rigorous 'Extreme Rigor' audit, we moved away from batch-averaged throughput to **individual operation latency distribution**, uncovering a performance profile that accurately represents single-threaded intrusive matching on modern ARM hardware.

## 2. Methodology

### 2.1 Individual Timing
We time every engine operation independently using `std::chrono::high_resolution_clock`. 
- **Refinement**: No block-averaging. This captures the true "jitter" and cost of individual map insertions and matching loops.
- **Hardware Constraint**: On Apple Silicon, the user-space counter operates at 24MHz (~41.6ns). Our results reflect this hardware quantization.

### 2.2 Spread-Crossing Matches
'Match' benchmarks are only valid if they generate trades. 
- **Validation**: Our matching benchmark crossed the spread for 20,000 operations and produced **20,197 trades on resting liquidity**.
- **Safety**: All matching was executed within the 5% Circuit Breaker band to ensure the engine was not in a halted state.

## 3. Results (Mac M1/Pro/Max)

| Metric | Adds (Limit) | Matches (Aggressive) | Cancels |
| :--- | :--- | :--- | :--- |
| **Average Latency** | **83.2 ns** | **131.7 ns** | **105.0 ns** |
| **P50 (Median)** | 83 ns | 125 ns | 83 ns |
| **P99** | 167 ns | 209 ns | 417 ns |
| **Throughput** | 12.02 M ops/s | 7.59 M ops/s | 9.52 M ops/s |

## 4. Architectural Analysis
- **Add Latency (83ns)**: Dominated by the `std::map` red-black tree insertion ($O(\log N)$) and `ObjectPool` retrieval.
- **Match Latency (131ns)**: Includes the `match()` loop, analytics updates (VWAP), and `Trade` history recording.
- **Cancel Latency (105ns)**: Includes monotonic ID lookup and intrusive list removal.

## 5. Conclusion
The engine delivers sub-150ns matching for aggressive orders and sub-100ns for limit order placement. These metrics, combined with 100% stability under 100,000-op fuzz stress, demonstrate a production-ready core suitable for professional HFT applications.
