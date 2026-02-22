#pragma once

#include <map>
#include <vector>
#include <functional>
#include "Order.h"
#include "Types.h"
#include "MemoryPool.h"
#include "IntrusiveList.h"

namespace OrderMatcher {

struct Trade {
    OrderId buyerId;
    OrderId sellerId;
    Price price;
    Qty qty;
    uint64_t timestamp;
};

using TradeCallback = std::function<void(const Trade&)>;

class OrderBook {
public:
    OrderBook();
    
    // Core API
    OrderId addOrder(ParticipantId participantId, Side side, Price price, Qty qty, OrderType type);
    bool cancelOrder(OrderId orderId);
    bool modifyOrder(OrderId orderId, Price newPrice, Qty newQty);
    
    void setTradeCallback(TradeCallback cb) { tradeCallback_ = cb; }

private:
    void match();
    bool checkSMP(const Order& incoming, const Order& resting);
    
    // Phase 5: Correct Sorted Maps
    // Bid: Highest Price First
    std::map<Price, IntrusiveList<Order>, std::greater<Price>> bids_;
    
    // Ask: Lowest Price First
    std::map<Price, IntrusiveList<Order>, std::less<Price>> asks_;
    
    // Order Lookup (O(1) indexing)
    std::vector<Order*> orderLookup_;
    
    // Object Pool for zero-allocation
    ObjectPool<Order> pool_;
    
    TradeCallback tradeCallback_;
    OrderId nextId_{1};
};

} // namespace OrderMatcher
