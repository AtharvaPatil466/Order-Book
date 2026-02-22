#include "OrderBook.h"
#include <iostream>
#include <chrono>

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace OrderMatcher {

// Initialize with sufficient capacity to avoid resizing during run
// 1 Million orders capacity for the pool and vector
constexpr size_t INITIAL_CAPACITY = 1000000;

OrderBook::OrderBook() : orderPool_(INITIAL_CAPACITY) {
    orderLookup_.resize(INITIAL_CAPACITY, nullptr);
}

void OrderBook::setTradeCallback(TradeCallback callback) {
    onTrade_ = std::move(callback);
}

void OrderBook::addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price, Quantity qty, OrderType type, Price stopPrice, Quantity displayQty) {
    if (UNLIKELY(halted_)) {
        otrStats_[participantId].rejectedOrders++;
        return;
    }
    
    otrStats_[participantId].ordersSubmitted++;

    if (UNLIKELY(referencePrice_ == 0)) referencePrice_ = price; // Set first price as reference

    if (type != OrderType::Market && !checkCircuitBreaker(price)) {
        halted_ = true;
        return;
    }
    Order* order = orderPool_.allocate();
    order->id = orderId;
    order->participantId = participantId;
    order->side = side;
    order->price = price;
    order->initialQty = qty;
    order->remainingQty = qty;
    order->type = type;
    order->stopPrice = stopPrice;
    order->displayQty = displayQty;
    order->timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    order->next = nullptr;
    order->prev = nullptr;

    // 2. Register for O(1) lookup
    if (UNLIKELY(orderId >= orderLookup_.size())) {
        orderLookup_.resize((orderId + 1) * 2, nullptr);
    }
    orderLookup_[orderId] = order;

    // 3. Attempt Match
    if (type == OrderType::FOK) {
        if (!checkLiquidity(side, price, qty, type)) {
            // Kill order immediately
            orderLookup_[orderId] = nullptr;
            orderPool_.deallocate(order);
            return;
        }
    }

    match(order);

    // Phase 6: Stop triggers
    if (lastTradePrice_ > 0) {
        checkStopOrders(lastTradePrice_);
    }

    // 4. If remaining quantity > 0 and not IOC/FOK, add to book
    if (LIKELY(order->remainingQty > 0)) {
        if (UNLIKELY(type == OrderType::IOC || type == OrderType::FOK)) {
            // Cancel remaining
            orderLookup_[orderId] = nullptr;
            orderPool_.deallocate(order);
        } else if (UNLIKELY(type == OrderType::Stop)) {
            // Add to stop tracking
            stopOrders_.push_back(order);
        } else {
            // Add to book
            if (type == OrderType::Iceberg) {
                // Initialize visible quantity
                order->visibleQty = std::min(order->remainingQty, order->displayQty);
            }
            
            if (side == Side::Buy) {
                bids_[price].push_back(order);
            } else {
                asks_[price].push_back(order);
            }
        }
    } else if (order->remainingQty == 0) {
        orderLookup_[orderId] = nullptr;
        orderPool_.deallocate(order);
    }
}

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty) {
    if (UNLIKELY(orderId >= orderLookup_.size() || orderLookup_[orderId] == nullptr)) {
        return false;
    }

    Order* order = orderLookup_[orderId];
    
    // Only support quantity reduction for now (preserves time priority)
    if (newQty < order->remainingQty) {
        order->remainingQty = newQty;
        // initialQty could also be updated but typically initial stays as record
        return true;
    }
    
    return false; // Price change or Qty increase should be Cancel-Replace
}

void OrderBook::cancelOrder(OrderId orderId) {
    if (UNLIKELY(orderId >= orderLookup_.size() || orderLookup_[orderId] == nullptr)) {
        return; // Order not found or already executed
    }

    Order* order = orderLookup_[orderId];
    otrStats_[order->participantId].ordersSubmitted++;
    
    // Remove from Intrusive List
    // We need to know which list it is in. 
    // Since we track bids and asks by price, we can find the list.
    // However, the intrusive list logic 'remove' handles the linking.
    // But we need to update the map if the list becomes empty?
    // The standard IntrusiveList remove just unlinks.
    // We need to access the list head/tail to check empty?
    // Actually, std::map value is the list head.
    
    // Optimization: We could store a pointer to the List in the Order, 
    // but that adds memory overhead.
    // Instead, we just unlink. The OrderList object in the map still "owns" the nodes.
    // Wait, if we just unlink 'order', the 'head' of the list might need updating.
    // Our IntrusiveList::remove logic handles head/tail updates IF it has access to the list object.
    // The current IntrusiveList::remove(Order*) needs the list object instance to update head/tail.
    // Refactor IntrusiveList: 
    // Option A: Order has pointer to parent List. (8 bytes overhead)
    // Option B: Search the map? (Slow O(log N))
    // Option C: When adding to map, if we knew we'd cancel, we'd want direct access.
    
    // CORRECT FIX: The IntrusiveList::remove logic I wrote earlier [does NOT] take the list object.
    // It takes the order. But `head` and `tail` are members of OrderList.
    // The previous implementation of `remove` was inside `OrderList`. 
    // So to call `remove`, I need the `OrderList` instance.
    
    if (order->side == Side::Buy) {
        auto it = bids_.find(order->price);
        if (it != bids_.end()) {
            it->second.remove(order);
            if (it->second.empty()) {
                bids_.erase(it);
            }
        }
    } else {
        auto it = asks_.find(order->price);
        if (it != asks_.end()) {
            it->second.remove(order);
            if (it->second.empty()) {
                asks_.erase(it);
            }
        }
    }

    orderLookup_[orderId] = nullptr;
    orderPool_.deallocate(order);
}

void OrderBook::match(Order* incoming) {
    while (incoming->remainingQty > 0) {
        if (incoming->side == Side::Buy) {
            if (UNLIKELY(asks_.empty())) break;
            
            auto bestAskIt = asks_.begin();
            Price bestAskPrice = bestAskIt->first;
            
            if (LIKELY(incoming->price < bestAskPrice && incoming->type != OrderType::Market)) {
                break; // No match
            }

            OrderList& level = bestAskIt->second;
            Order* bookOrder = level.front(); 
            
            // Phase 5: Self-Match Prevention
            if (UNLIKELY(checkSMP(*incoming, *bookOrder))) {
                incoming->remainingQty = 0;
                break;
            }
            
            // Phase 6: Iceberg visible quantity handling
            Quantity availableInBook = (bookOrder->type == OrderType::Iceberg) ? bookOrder->visibleQty : bookOrder->remainingQty;
            Quantity fillQty = std::min(incoming->remainingQty, availableInBook);
            
            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

            lastTradePrice_ = bestAskPrice;
            updateAnalytics(bestAskPrice, fillQty, bookOrder->participantId, incoming->participantId);
                Trade t = {
                    nextTradeId_++,
                    incoming->id, bookOrder->id,
                    incoming->participantId, bookOrder->participantId,
                    bestAskPrice, fillQty,
                    static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
                };
                tradeHistory_.push_back(t);
                if (onTrade_) onTrade_(t);
            
            if (UNLIKELY(bookOrder->remainingQty == 0)) {
                level.remove(bookOrder);
                orderLookup_[bookOrder->id] = nullptr;
                orderPool_.deallocate(bookOrder);
                
                if (UNLIKELY(level.empty())) {
                    asks_.erase(bestAskIt);
                }
            } else if (UNLIKELY(bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0)) {
                // Refresh Iceberg portion and move to back (lose priority)
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                level.remove(bookOrder);
                level.push_back(bookOrder);
            }

        } else {
             if (UNLIKELY(bids_.empty())) break;
            
            auto bestBidIt = bids_.begin();
            Price bestBidPrice = bestBidIt->first;
            
            if (LIKELY(incoming->price > bestBidPrice && incoming->type != OrderType::Market)) {
                break; // No match
            }
            
            OrderList& level = bestBidIt->second;
            Order* bookOrder = level.front();
            
            // Phase 5: Self-Match Prevention
            if (UNLIKELY(checkSMP(*incoming, *bookOrder))) {
                incoming->remainingQty = 0;
                break;
            }
            
            // Phase 6: Iceberg
            Quantity availableInBook = (bookOrder->type == OrderType::Iceberg) ? bookOrder->visibleQty : bookOrder->remainingQty;
            Quantity fillQty = std::min(incoming->remainingQty, availableInBook);
            
            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;
            
            lastTradePrice_ = bestBidPrice;
            updateAnalytics(bestBidPrice, fillQty, bookOrder->participantId, incoming->participantId);
                Trade t = {
                    nextTradeId_++,
                    bookOrder->id, incoming->id,
                    bookOrder->participantId, incoming->participantId,
                    bestBidPrice, fillQty,
                    static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
                };
                tradeHistory_.push_back(t);
                if (onTrade_) onTrade_(t);

            if (UNLIKELY(bookOrder->remainingQty == 0)) {
                level.remove(bookOrder);
                orderLookup_[bookOrder->id] = nullptr;
                orderPool_.deallocate(bookOrder);
                
                if (UNLIKELY(level.empty())) {
                    bids_.erase(bestBidIt);
                }
            } else if (UNLIKELY(bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0)) {
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                level.remove(bookOrder);
                level.push_back(bookOrder);
            }
        }
    }
}

const Order* OrderBook::getOrder(OrderId orderId) const {
    if (orderId < orderLookup_.size()) {
        return orderLookup_[orderId];
    }
    return nullptr;
}

size_t OrderBook::getBidLevelsCount() const {
    return bids_.size();
}

size_t OrderBook::getAskLevelsCount() const {
    return asks_.size();
}

bool OrderBook::checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const {
    Quantity remainingToFill = qty;
    
    if (side == Side::Buy) {
        for (const auto& [levelPrice, level] : asks_) {
            if (type != OrderType::Market && levelPrice > price) break;
            
            // For FOK/IOC, we only scan until we reach the required qty
            for (Order* bookOrder = level.front(); bookOrder != nullptr; bookOrder = bookOrder->next) {
                remainingToFill -= std::min(remainingToFill, bookOrder->remainingQty);
                if (remainingToFill == 0) return true;
            }
        }
    } else {
        for (const auto& [levelPrice, level] : bids_) {
            if (type != OrderType::Market && levelPrice < price) break;
            
            for (Order* bookOrder = level.front(); bookOrder != nullptr; bookOrder = bookOrder->next) {
                remainingToFill -= std::min(remainingToFill, bookOrder->remainingQty);
                if (remainingToFill == 0) return true;
            }
        }
    }
    
    return remainingToFill == 0;
}

bool OrderBook::checkSMP(const Order& incoming, const Order& resting) const {
    return incoming.participantId == resting.participantId;
}

Price OrderBook::getBestBid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::getBestAsk() const {
    return asks_.empty() ? std::numeric_limits<Price>::max() : asks_.begin()->first;
}

void OrderBook::updateAnalytics(Price price, Quantity qty, ParticipantId p1, ParticipantId p2) {
    // VWAP
    double tradeValue = static_cast<double>(price) * qty;
    vwap_ = (vwap_ * totalQty_ + tradeValue) / (totalQty_ + qty);
    totalQty_ += qty;
    
    // TWAP (Simple version: average of trade prices)
    cumulativePrice_ += price;
    priceUpdates_++;

    // OTR
    if (p1 > 0) otrStats_[p1].tradesExecuted++;
    if (p2 > 0) otrStats_[p2].tradesExecuted++;
}

void OrderBook::uncross() {
    if (bids_.empty() || asks_.empty()) return;

    Price bestUncrossPrice = 0;
    Quantity maxVolume = 0;

    // Collect all unique prices from bids and asks
    std::vector<Price> prices;
    for (const auto& [p, _] : bids_) prices.push_back(p);
    for (const auto& [p, _] : asks_) prices.push_back(p);
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

    for (Price p : prices) {
        Quantity cumBuy = 0;
        Quantity cumSell = 0;

        for (const auto& [bp, list] : bids_) {
            if (bp >= p) {
                for (Order* o = list.front(); o != nullptr; o = o->next) cumBuy += o->remainingQty;
            }
        }
        for (const auto& [ap, list] : asks_) {
            if (ap <= p) {
                for (Order* o = list.front(); o != nullptr; o = o->next) cumSell += o->remainingQty;
            }
        }

        Quantity volume = std::min(cumBuy, cumSell);
        if (volume > maxVolume) {
            maxVolume = volume;
            bestUncrossPrice = p;
        }
    }

    if (maxVolume > 0) {
        // In a real system, we'd execute trades here. 
        // For now, we report the price.
        lastTradePrice_ = bestUncrossPrice;
    }
}

bool OrderBook::checkCircuitBreaker(Price price) {
    if (referencePrice_ == 0) return true;
    
    double deviation = std::abs(static_cast<double>(price - referencePrice_)) / referencePrice_;
    return deviation <= 0.05; // 5% limit
}

void OrderBook::checkStopOrders(Price lastTradePrice) {
    if (UNLIKELY(stopOrders_.empty())) return;

    for (auto it = stopOrders_.begin(); it != stopOrders_.end(); ) {
        Order* order = *it;
        bool triggered = false;
        
        if (order->side == Side::Buy) {
            if (lastTradePrice >= order->stopPrice) triggered = true;
        } else {
            if (lastTradePrice <= order->stopPrice) triggered = true;
        }

        if (triggered) {
            order->type = OrderType::Limit; 
            it = stopOrders_.erase(it);
            match(order);
            
            if (order->remainingQty > 0) {
                if (order->side == Side::Buy) {
                    bids_[order->price].push_back(order);
                } else {
                    asks_[order->price].push_back(order);
                }
            } else {
                orderLookup_[order->id] = nullptr;
                orderPool_.deallocate(order);
            }
        } else {
            ++it;
        }
    }
}

} // namespace OrderMatcher
