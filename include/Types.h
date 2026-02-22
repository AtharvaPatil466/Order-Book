#pragma once

#include <cstdint>
#include <limits>

namespace OrderMatcher {

using OrderId = uint64_t;
using ParticipantId = uint64_t;
using Price = int64_t;
using Quantity = uint64_t;

enum class Side : uint8_t {
    Buy,
    Sell
};

enum class OrderType : uint8_t {
    Limit,
    Market,
    IOC, // Immediate or Cancel
    FOK, // Fill or Kill
    Stop,
    Iceberg
};

// Fixed-point price constants (4 decimal places)
constexpr int64_t PRICE_PRECISION = 10000;

inline double toDouble(Price p) {
    return static_cast<double>(p) / PRICE_PRECISION;
}

inline Price toPrice(double p) {
    return static_cast<Price>(p * PRICE_PRECISION + 0.5);
}

} // namespace OrderMatcher
