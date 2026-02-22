#include "MatchingEngine.h"
#include <iostream>

namespace OrderMatcher {

MatchingEngine::MatchingEngine() {
    // Initialize callbacks
    orderBook_.setTradeCallback([]([[maybe_unused]] const Trade& trade) {
        // In a real system, push to lock-free queue for logging/network
        // For now, simple print to stdout (not low latency, but for verification)
        // We will replace this with RingBuffer later
        // std::cout << "Trade Executed: " << trade.quantity << " @ " << trade.price << std::endl;
    });
}

MatchingEngine::~MatchingEngine() {
    stop();
}

void MatchingEngine::start() {
    running_ = true;
    // Thread pinning logic would go here or in the worker thread loop
}

void MatchingEngine::stop() {
    running_ = false;
}

void MatchingEngine::processOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type, Price stopPrice, Quantity displayQty) {
    if (!running_) return;
    orderBook_.addOrder(orderId, participantId, side, price, qty, type, stopPrice, displayQty);
}

void MatchingEngine::cancelOrder(OrderId orderId) {
    if (!running_) return;
    orderBook_.cancelOrder(orderId);
}

} // namespace OrderMatcher
