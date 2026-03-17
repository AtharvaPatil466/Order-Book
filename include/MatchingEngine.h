#pragma once

#include "OrderBook.h"
#include "Journal.h"
#include "RingBuffer.h"
#include "FIXParser.h"
#include "Utils.h"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <memory>

namespace OrderMatcher {

// Message struct for async ring buffer processing
struct OrderRequest {
    enum class Type : uint8_t {
        NewOrder = 1,
        Cancel = 2,
        Modify = 3,
        CancelReplace = 4,
        KillSwitch = 5,
        Shutdown = 6
    };

    Type type;
    SymbolId symbolId;
    OrderId orderId;
    ParticipantId participantId;
    Side side;
    Price price;
    Quantity qty;
    OrderType orderType;
    Price stopPrice;
    Quantity displayQty;
    TimeInForce tif;
    uint64_t expiryTime;
    Price stopLimitPrice;
    PegType pegType;
    Price pegOffset;
    Price trailAmount;
    Quantity minQty;
    bool hidden;

    // For modify/cancel-replace
    Price newPrice;
    Quantity newQty;
};

class MatchingEngine {
public:
    MatchingEngine();
    ~MatchingEngine();

    // Synchronous mode (direct calls, no threading)
    void start();
    void stop();

    // Async mode: spawns a dedicated worker thread with optional CPU pinning
    void startAsync(int coreId = -1, size_t queueSize = 8192);
    void stopAsync();

    // Wait until all queued requests have been processed
    void waitForDrain();

    bool isAsync() const { return async_; }

    // Symbol management
    void addSymbol(SymbolId symbolId, MatchAlgorithm algo = MatchAlgorithm::PriceTime);
    OrderBook* getOrderBook(SymbolId symbolId);
    const OrderBook* getOrderBook(SymbolId symbolId) const;

    // Multi-symbol gateway (sync mode: direct call; async mode: enqueue)
    void processOrder(SymbolId symbolId, OrderId orderId, ParticipantId participantId,
                      Side side, Price price, Quantity qty, OrderType type,
                      Price stopPrice = 0, Quantity displayQty = 0,
                      TimeInForce tif = TimeInForce::GTC, uint64_t expiryTime = 0,
                      Price stopLimitPrice = 0,
                      PegType pegType = PegType::None, Price pegOffset = 0,
                      Price trailAmount = 0, Quantity minQty = 0, bool hidden = false);

    void cancelOrder(SymbolId symbolId, OrderId orderId);
    bool modifyOrder(SymbolId symbolId, OrderId orderId, Quantity newQty);
    bool cancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice, Quantity newQty);

    // Kill switch: cancel all orders for a participant across ALL symbols
    uint64_t killSwitch(ParticipantId participantId);

    // Risk management
    void setRiskLimits(SymbolId symbolId, ParticipantId participantId, const RiskLimits& limits);

    // Market data
    MarketDataSnapshot getSnapshot(SymbolId symbolId, size_t depth = 10);

    // Auction
    void uncross(SymbolId symbolId);

    // Time management
    void expireOrders(uint64_t currentTime);

    // Journal/Persistence
    void enableJournal(const std::string& path);

    // Replay journal to rebuild order book state (crash recovery)
    size_t replayJournal();

    // Write full snapshot of all active orders to journal (checkpoint)
    void checkpoint();

    // Async stats
    uint64_t getSubmittedCount() const { return submitted_.load(std::memory_order_relaxed); }
    uint64_t getProcessedCount() const { return processed_.load(std::memory_order_relaxed); }

    // ─── FIX protocol gateway ─────────────────────────────────────────────────
    // Process a raw FIX message string and route to appropriate handler
    void processFIXMessage(const std::string& rawFix);

    // ─── Legacy single-symbol interface (backward compat, uses symbol 0) ───
    void processOrder(OrderId orderId, ParticipantId participantId, Side side,
                      Price price, Quantity qty, OrderType type,
                      Price stopPrice = 0, Quantity displayQty = 0);
    void cancelOrder(OrderId orderId);
    void uncross() { uncross(0); }
    double getVWAP() const;

private:
    void ensureDefaultSymbol();
    void workerLoop(int coreId);
    void processRequest(const OrderRequest& req);

    std::unordered_map<SymbolId, std::unique_ptr<OrderBook>> books_;
    std::atomic<bool> running_{false};
    std::unique_ptr<Journal> journal_;

    // Async mode
    bool async_{false};
    std::unique_ptr<RingBuffer<OrderRequest>> requestQueue_;
    std::thread workerThread_;
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> processed_{0};
};

} // namespace OrderMatcher
