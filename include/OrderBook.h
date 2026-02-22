#pragma once

#include "Order.h"
#include "IntrusiveList.h"
#include "MemoryPool.h"
#include <map>
#include <vector>
#include <functional>

namespace OrderMatcher {

struct Trade {
    uint64_t tradeId;
    OrderId buyOrderId;
    OrderId sellOrderId;
    ParticipantId buyerId;
    ParticipantId sellerId;
    Price price;
    Quantity quantity;
    uint64_t timestamp;
};

// Callback for trade execution
using TradeCallback = std::function<void(const Trade&)>;

struct ParticipantStats {
    uint64_t ordersSubmitted = 0;
    uint64_t tradesExecuted = 0;
    uint64_t rejectedOrders = 0;
    
    double getOTR() const {
        return static_cast<double>(ordersSubmitted) / std::max(1ULL, tradesExecuted);
    }
};

class OrderBook {
public:
    OrderBook();

    // Core Actions
    void addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type, Price stopPrice = 0, Quantity displayQty = 0);
    void cancelOrder(OrderId orderId);
    bool modifyOrder(OrderId orderId, Quantity newQty); 
    void uncross();
    double getVWAP() const { return vwap_; }
    uint64_t getTradeCount() const { return priceUpdates_; }
    const std::vector<Trade>& getTradeHistory() const { return tradeHistory_; }
    void clearTradeHistory() { tradeHistory_.clear(); }
    void resetStatus() { 
        halted_ = false; 
        referencePrice_ = 0; 
        priceUpdates_ = 0;
        tradeHistory_.clear();
    }
    ParticipantStats getParticipantStats(ParticipantId id) const {
        auto it = otrStats_.find(id);
        return it != otrStats_.end() ? it->second : ParticipantStats{};
    }

    // Getters for testing/verification
    const Order* getOrder(OrderId orderId) const;
    size_t getBidLevelsCount() const;
    size_t getAskLevelsCount() const;

    // Set callback to report trades
    void setTradeCallback(TradeCallback callback);

    Price getBestBid() const;
    Price getBestAsk() const;

    double getOTR(ParticipantId p) const {
        auto it = otrStats_.find(p);
        return (it != otrStats_.end()) ? it->second.getOTR() : 0.0;
    }

private:
    void match(Order* order);
    bool canMatch(Side side, Price price) const;
    bool checkSMP(const Order& incoming, const Order& resting) const;
    bool checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const;
    
    void checkStopOrders(Price lastTradePrice);
    void updateAnalytics(Price price, Quantity qty, ParticipantId p1 = 0, ParticipantId p2 = 0);
    bool checkCircuitBreaker(Price price);
    
    // Core Data Structures
    // Bids: Descending Price (Higher is better)
    std::map<Price, OrderList, std::greater<Price>> bids_;
    
    // Asks: Ascending Price (Lower is better)
    std::map<Price, OrderList, std::less<Price>> asks_;

    // Pending Stop Orders
    std::vector<Order*> stopOrders_;
    
    // O(1) Lookup: Monotonic ID -> Order Pointer
    std::vector<Order*> orderLookup_;
    
    // Memory Management
    ObjectPool<Order> orderPool_;
    
    // Output
    TradeCallback onTrade_;
    OrderId nextId_{1};
    uint64_t nextTradeId_{1};
    Price lastTradePrice_{0};
    
    // Analytics
    double vwap_ = 0.0;
    Quantity totalQty_ = 0;
    double cumulativePrice_ = 0.0;
    size_t priceUpdates_ = 0;
    
    // Safety
    Price referencePrice_{0}; // Anchor for circuit breaker
    bool halted_{false};
    
    std::unordered_map<ParticipantId, ParticipantStats> otrStats_;
    std::vector<Trade> tradeHistory_;
};

} // namespace OrderMatcher
