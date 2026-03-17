#pragma once

#include "Order.h"
#include "IntrusiveList.h"
#include "MemoryPool.h"
#include <map>
#include <vector>
#include <functional>
#include <unordered_map>

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
    SymbolId symbolId = 0;
};

// Market Data: single price level
struct PriceLevel {
    Price price;
    Quantity totalQuantity;
    uint32_t orderCount;
};

// Market Data: L2 Depth Snapshot
struct MarketDataSnapshot {
    SymbolId symbolId;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    Price lastTradePrice;
    Quantity lastTradeQty;
    uint64_t timestamp;
};

// Market Data: incremental update
struct MarketDataUpdate {
    enum class Action : uint8_t { Add, Modify, Delete };
    Action action;
    Side side;
    PriceLevel level;
    uint64_t timestamp;
};

// Order status notification
struct OrderUpdate {
    OrderId orderId;
    OrderStatus status;
    Quantity filledQty;
    Quantity remainingQty;
    Price lastFillPrice;
    RejectReason rejectReason = RejectReason::None;
    uint64_t timestamp;
};

// Pre-trade risk limits per participant
struct RiskLimits {
    Quantity maxOrderSize = 0;       // 0 = no limit
    Price maxOrderNotional = 0;      // 0 = no limit
    Quantity maxPositionSize = 0;    // 0 = no limit
};

// Callbacks
using TradeCallback = std::function<void(const Trade&)>;
using OrderUpdateCallback = std::function<void(const OrderUpdate&)>;
using MarketDataCallback = std::function<void(const MarketDataUpdate&)>;

struct ParticipantStats {
    uint64_t ordersSubmitted = 0;
    uint64_t tradesExecuted = 0;
    uint64_t rejectedOrders = 0;
    int64_t netPosition = 0;

    double getOTR() const {
        return static_cast<double>(ordersSubmitted) / std::max(1ULL, tradesExecuted);
    }
};

class OrderBook {
public:
    explicit OrderBook(SymbolId symbolId = 0, MatchAlgorithm algo = MatchAlgorithm::PriceTime);

    // Core Actions
    void addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type,
                  Price stopPrice = 0, Quantity displayQty = 0,
                  TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                  Price stopLimitPrice = 0,
                  PegType pegType = PegType::None, Price pegOffset = 0,
                  Price trailAmount = 0, Quantity minQty = 0, bool hidden = false);
    void cancelOrder(OrderId orderId);
    bool modifyOrder(OrderId orderId, Quantity newQty);

    // Cancel/Replace: full amendment (price change loses time priority)
    bool cancelReplace(OrderId orderId, Price newPrice, Quantity newQty);

    // Kill switch: cancel all orders for a participant
    uint64_t cancelAllForParticipant(ParticipantId participantId);

    // Auction
    void uncross();

    // Time-based expiry: call periodically to expire GTD/DAY orders
    void expireOrders(uint64_t currentTime);

    // Market Data
    MarketDataSnapshot getSnapshot(size_t depth = 10) const;

    // Risk Management
    void setRiskLimits(ParticipantId participantId, const RiskLimits& limits);

    // Analytics
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

    // Configurable circuit breaker
    void setCircuitBreakerThreshold(double threshold) { cbThreshold_ = threshold; }
    double getCircuitBreakerThreshold() const { return cbThreshold_; }
    void setReferencePrice(Price price) { referencePrice_ = price; }
    ParticipantStats getParticipantStats(ParticipantId id) const {
        auto it = otrStats_.find(id);
        return it != otrStats_.end() ? it->second : ParticipantStats{};
    }

    // Depth limit: max price levels per side (0 = unlimited)
    void setMaxDepth(size_t maxLevels) { maxDepthPerSide_ = maxLevels; }
    size_t getMaxDepth() const { return maxDepthPerSide_; }

    // Get all active orders (for snapshots/inspection)
    std::vector<const Order*> getAllOrders() const;

    // Getters
    const Order* getOrder(OrderId orderId) const;
    size_t getBidLevelsCount() const;
    size_t getAskLevelsCount() const;
    SymbolId getSymbolId() const { return symbolId_; }
    bool isHalted() const { return halted_; }

    // Callbacks
    void setTradeCallback(TradeCallback callback);
    void setOrderUpdateCallback(OrderUpdateCallback callback);
    void setMarketDataCallback(MarketDataCallback callback);

    Price getBestBid() const;
    Price getBestAsk() const;
    Price getMidPrice() const;

    double getOTR(ParticipantId p) const {
        auto it = otrStats_.find(p);
        return (it != otrStats_.end()) ? it->second.getOTR() : 0.0;
    }

    void setMatchAlgorithm(MatchAlgorithm algo) { matchAlgorithm_ = algo; }

private:
    void match(Order* order);
    void matchProRata(Order* order);
    bool checkSMP(const Order& incoming, const Order& resting) const;
    bool checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const;
    bool checkMinQty(Side side, Price price, Quantity minQty) const;

    void checkStopOrders(Price lastTradePrice);
    void updateTrailingStops(Price lastTradePrice);
    void updatePeggedOrders();
    void updateAnalytics(Price price, Quantity qty, ParticipantId p1 = 0, ParticipantId p2 = 0);
    bool checkCircuitBreaker(Price price);
    bool checkRiskLimits(ParticipantId participantId, Price price, Quantity qty);

    void notifyOrderUpdate(OrderId orderId, OrderStatus status, Quantity filledQty, Quantity remainingQty,
                           Price lastFillPrice = 0, RejectReason reason = RejectReason::None);
    void notifyMarketData(MarketDataUpdate::Action action, Side side, Price price);

    void removeFromBook(Order* order);
    bool addToBook(Order* order);  // returns false if depth limit exceeded
    bool canAddToBook(const Order* order) const;

    // Bids: Descending Price
    std::map<Price, OrderList, std::greater<Price>> bids_;
    // Asks: Ascending Price
    std::map<Price, OrderList, std::less<Price>> asks_;

    // Pending orders by type
    std::vector<Order*> stopOrders_;
    std::vector<Order*> trailingStopOrders_;
    std::vector<Order*> peggedOrders_;

    // O(1) Lookup
    std::vector<Order*> orderLookup_;
    ObjectPool<Order> orderPool_;

    // Callbacks
    TradeCallback onTrade_;
    OrderUpdateCallback onOrderUpdate_;
    MarketDataCallback onMarketData_;

    // Identity
    SymbolId symbolId_;
    MatchAlgorithm matchAlgorithm_;

    uint64_t nextTradeId_{1};
    Price lastTradePrice_{0};
    Quantity lastTradeQty_{0};

    // Analytics
    double vwap_ = 0.0;
    Quantity totalQty_ = 0;
    double cumulativePrice_ = 0.0;
    size_t priceUpdates_ = 0;

    // Safety
    Price referencePrice_{0};
    bool halted_{false};
    size_t maxDepthPerSide_{0};  // 0 = unlimited
    double cbThreshold_{0.05};   // Circuit breaker threshold (default 5%)

    std::unordered_map<ParticipantId, ParticipantStats> otrStats_;
    std::unordered_map<ParticipantId, RiskLimits> riskLimits_;
    std::vector<Trade> tradeHistory_;
};

} // namespace OrderMatcher
