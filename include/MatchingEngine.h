#pragma once

#include "OrderBook.h"
#include "RingBuffer.h"
#include <thread>
#include <atomic>

namespace OrderMatcher {

class MatchingEngine {
public:
    MatchingEngine();
    ~MatchingEngine();

    void start();
    void stop();

    // Gateway Interface
    void processOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type, Price stopPrice = 0, Quantity displayQty = 0);
    void cancelOrder(OrderId orderId);
    void uncross() { orderBook_.uncross(); }
    double getVWAP() const { return orderBook_.getVWAP(); }

private:
    OrderBook orderBook_;
    std::atomic<bool> running_{false};
    
    // In a real system, we might have a ring buffer for incoming orders too
    // For now, we call directly from the main thread or gateway thread
};

} // namespace OrderMatcher
