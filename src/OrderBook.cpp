#include "OrderBook.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

namespace OrderMatcher {

constexpr size_t INITIAL_CAPACITY = 1000000;

static uint64_t nowNs() {
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

OrderBook::OrderBook(SymbolId symbolId, MatchAlgorithm algo)
    : orderPool_(INITIAL_CAPACITY), symbolId_(symbolId), matchAlgorithm_(algo) {
    orderLookup_.resize(INITIAL_CAPACITY, nullptr);
}

void OrderBook::setTradeCallback(TradeCallback callback) {
    onTrade_ = std::move(callback);
}

void OrderBook::setOrderUpdateCallback(OrderUpdateCallback callback) {
    onOrderUpdate_ = std::move(callback);
}

void OrderBook::setMarketDataCallback(MarketDataCallback callback) {
    onMarketData_ = std::move(callback);
}

// ─── Notifications ───────────────────────────────────────────────────────────

void OrderBook::notifyOrderUpdate(OrderId orderId, OrderStatus status, Quantity filledQty,
                                   Quantity remainingQty, Price lastFillPrice, RejectReason reason) {
    if (!onOrderUpdate_) return;
    OrderUpdate u{};
    u.orderId = orderId;
    u.status = status;
    u.filledQty = filledQty;
    u.remainingQty = remainingQty;
    u.lastFillPrice = lastFillPrice;
    u.rejectReason = reason;
    u.timestamp = nowNs();
    onOrderUpdate_(u);
}

void OrderBook::notifyMarketData(MarketDataUpdate::Action action, Side side, Price price) {
    if (!onMarketData_) return;

    MarketDataUpdate update{};
    update.action = action;
    update.side = side;
    update.timestamp = nowNs();

    PriceLevel lvl{};
    lvl.price = price;

    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            for (Order* o = it->second.front(); o; o = o->next) {
                if (!o->isHidden) {
                    lvl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                    lvl.orderCount++;
                }
            }
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            for (Order* o = it->second.front(); o; o = o->next) {
                if (!o->isHidden) {
                    lvl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                    lvl.orderCount++;
                }
            }
        }
    }

    update.level = lvl;
    onMarketData_(update);
}

// ─── Book helpers ────────────────────────────────────────────────────────────

bool OrderBook::canAddToBook(const Order* order) const {
    if (maxDepthPerSide_ == 0) return true;
    if (order->side == Side::Buy)
        return bids_.count(order->price) > 0 || bids_.size() < maxDepthPerSide_;
    else
        return asks_.count(order->price) > 0 || asks_.size() < maxDepthPerSide_;
}

bool OrderBook::addToBook(Order* order) {
    if (!canAddToBook(order)) return false;
    if (order->side == Side::Buy)
        bids_[order->price].push_back(order);
    else
        asks_[order->price].push_back(order);
    return true;
}

void OrderBook::removeFromBook(Order* order) {
    if (order->side == Side::Buy) {
        auto it = bids_.find(order->price);
        if (it != bids_.end()) {
            it->second.remove(order);
            if (it->second.empty()) bids_.erase(it);
        }
    } else {
        auto it = asks_.find(order->price);
        if (it != asks_.end()) {
            it->second.remove(order);
            if (it->second.empty()) asks_.erase(it);
        }
    }
}

// ─── Risk & Validation ──────────────────────────────────────────────────────

bool OrderBook::checkRiskLimits(ParticipantId participantId, Price price, Quantity qty) {
    auto it = riskLimits_.find(participantId);
    if (it == riskLimits_.end()) return true;

    const auto& lim = it->second;

    if (lim.maxOrderSize > 0 && qty > lim.maxOrderSize) return false;

    if (lim.maxOrderNotional > 0) {
        Price notional = price * static_cast<Price>(qty) / PRICE_PRECISION;
        if (notional > lim.maxOrderNotional) return false;
    }

    if (lim.maxPositionSize > 0) {
        auto sit = otrStats_.find(participantId);
        if (sit != otrStats_.end()) {
            int64_t projected = sit->second.netPosition + static_cast<int64_t>(qty);
            if (static_cast<uint64_t>(std::abs(projected)) > lim.maxPositionSize) return false;
        }
    }

    return true;
}

void OrderBook::setRiskLimits(ParticipantId participantId, const RiskLimits& limits) {
    riskLimits_[participantId] = limits;
}

bool OrderBook::checkCircuitBreaker(Price price) {
    if (referencePrice_ == 0) return true;
    double deviation = std::abs(static_cast<double>(price - referencePrice_)) / referencePrice_;
    return deviation <= cbThreshold_;
}

bool OrderBook::checkSMP(const Order& incoming, const Order& resting) const {
    return incoming.participantId == resting.participantId;
}

bool OrderBook::checkLiquidity(Side side, Price price, Quantity qty, OrderType type) const {
    Quantity remaining = qty;

    if (side == Side::Buy) {
        for (const auto& [levelPrice, level] : asks_) {
            if (type != OrderType::Market && levelPrice > price) break;
            for (Order* o = level.front(); o; o = o->next) {
                remaining -= std::min(remaining, o->remainingQty);
                if (remaining == 0) return true;
            }
        }
    } else {
        for (const auto& [levelPrice, level] : bids_) {
            if (type != OrderType::Market && levelPrice < price) break;
            for (Order* o = level.front(); o; o = o->next) {
                remaining -= std::min(remaining, o->remainingQty);
                if (remaining == 0) return true;
            }
        }
    }

    return remaining == 0;
}

bool OrderBook::checkMinQty(Side side, Price price, Quantity minQty) const {
    Quantity available = 0;

    if (side == Side::Buy) {
        for (const auto& [levelPrice, level] : asks_) {
            if (levelPrice > price) break;
            for (Order* o = level.front(); o; o = o->next) {
                available += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                if (available >= minQty) return true;
            }
        }
    } else {
        for (const auto& [levelPrice, level] : bids_) {
            if (levelPrice < price) break;
            for (Order* o = level.front(); o; o = o->next) {
                available += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                if (available >= minQty) return true;
            }
        }
    }

    return false;
}

// ─── addOrder ────────────────────────────────────────────────────────────────

void OrderBook::addOrder(OrderId orderId, ParticipantId participantId, Side side, Price price,
                          Quantity qty, OrderType type, Price stopPrice, Quantity displayQty,
                          TimeInForce tif, uint64_t expiryTime, Price stopLimitPrice,
                          PegType pegType, Price pegOffset, Price trailAmount,
                          Quantity minQty, bool hidden) {
    // --- Reject if halted ---
    if (UNLIKELY(halted_)) {
        otrStats_[participantId].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::CircuitBreakerHalt);
        return;
    }

    // --- Input validation ---
    if (UNLIKELY(qty == 0)) {
        otrStats_[participantId].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, 0, 0, RejectReason::InvalidQuantity);
        return;
    }

    if (UNLIKELY(type != OrderType::Market && price <= 0
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop)) {
        otrStats_[participantId].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::InvalidPrice);
        return;
    }

    // --- Pre-trade risk checks ---
    if (UNLIKELY(!checkRiskLimits(participantId, price, qty))) {
        otrStats_[participantId].rejectedOrders++;
        notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::RiskLimitBreached);
        return;
    }

    otrStats_[participantId].ordersSubmitted++;

    // --- Reference price for circuit breaker ---
    if (UNLIKELY(referencePrice_ == 0 && type != OrderType::Market
                 && type != OrderType::Stop && type != OrderType::StopLimit
                 && type != OrderType::TrailingStop)) {
        referencePrice_ = price;
    }

    // --- Circuit breaker check ---
    if (type == OrderType::Limit || type == OrderType::IOC || type == OrderType::FOK
        || type == OrderType::PostOnly || type == OrderType::Iceberg || type == OrderType::Hidden) {
        if (!checkCircuitBreaker(price)) {
            halted_ = true;
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::CircuitBreakerHalt);
            return;
        }
    }

    // --- Post-Only: reject if would cross the spread ---
    if (UNLIKELY(type == OrderType::PostOnly)) {
        bool wouldCross = false;
        if (side == Side::Buy)
            wouldCross = !asks_.empty() && price >= asks_.begin()->first;
        else
            wouldCross = !bids_.empty() && price <= bids_.begin()->first;

        if (wouldCross) {
            otrStats_[participantId].rejectedOrders++;
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::PostOnlyWouldCross);
            return;
        }
    }

    // --- Allocate order ---
    Order* order = orderPool_.allocate();
    order->id = orderId;
    order->participantId = participantId;
    order->side = side;
    order->price = price;
    order->initialQty = qty;
    order->remainingQty = qty;
    order->type = type;
    order->status = OrderStatus::Accepted;
    order->timeInForce = tif;
    order->expiryTime = expiryTime;
    order->stopPrice = stopPrice;
    order->stopLimitPrice = stopLimitPrice;
    order->displayQty = displayQty;
    order->visibleQty = 0;
    order->pegType = pegType;
    order->pegOffset = pegOffset;
    order->trailAmount = trailAmount;
    order->trailRefPrice = 0;
    order->minQty = minQty;
    order->isHidden = hidden || (type == OrderType::Hidden);
    order->isStopTriggered = false;
    order->symbolId = symbolId_;
    order->timestamp = nowNs();
    order->next = nullptr;
    order->prev = nullptr;

    // --- Register for O(1) lookup ---
    if (UNLIKELY(orderId >= orderLookup_.size())) {
        orderLookup_.resize((orderId + 1) * 2, nullptr);
    }
    orderLookup_[orderId] = order;

    notifyOrderUpdate(orderId, OrderStatus::Accepted, 0, qty);

    // --- Stop / StopLimit: park until triggered ---
    if (type == OrderType::Stop || type == OrderType::StopLimit) {
        stopOrders_.push_back(order);
        return;
    }

    // --- Trailing Stop: park and initialize reference ---
    if (type == OrderType::TrailingStop) {
        if (side == Side::Buy) {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            order->stopPrice = order->trailRefPrice + trailAmount;
        } else {
            order->trailRefPrice = lastTradePrice_ > 0 ? lastTradePrice_ : price;
            order->stopPrice = order->trailRefPrice - trailAmount;
        }
        trailingStopOrders_.push_back(order);
        return;
    }

    // --- Pegged: compute price from reference and rest ---
    if (type == OrderType::Pegged) {
        Price pegPrice = price; // fallback
        if (pegType == PegType::MidPeg) {
            Price mid = getMidPrice();
            if (mid > 0) pegPrice = mid + pegOffset;
        } else if (pegType == PegType::PrimaryPeg) {
            if (side == Side::Buy) {
                Price bb = getBestBid();
                if (bb > 0) pegPrice = bb + pegOffset;
            } else {
                Price ba = getBestAsk();
                if (ba < std::numeric_limits<Price>::max()) pegPrice = ba + pegOffset;
            }
        }
        order->price = pegPrice;
        peggedOrders_.push_back(order);
        addToBook(order);
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Add, side, order->price);
        return;
    }

    // --- FOK: require full liquidity ---
    if (type == OrderType::FOK) {
        if (!checkLiquidity(side, price, qty, type)) {
            orderLookup_[orderId] = nullptr;
            orderPool_.deallocate(order);
            notifyOrderUpdate(orderId, OrderStatus::Rejected, 0, qty, 0, RejectReason::FOKInsufficientLiquidity);
            return;
        }
    }

    // --- Min quantity check ---
    if (UNLIKELY(minQty > 0 && type != OrderType::FOK)) {
        if (!checkMinQty(side, price, minQty)) {
            if (type == OrderType::IOC) {
                orderLookup_[orderId] = nullptr;
                orderPool_.deallocate(order);
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, 0, qty);
                return;
            }
            // For limit orders, rest in book without matching (will match later)
            addToBook(order);
            if (!order->isHidden)
                notifyMarketData(MarketDataUpdate::Action::Add, side, price);
            return;
        }
    }

    // --- Match ---
    if (matchAlgorithm_ == MatchAlgorithm::ProRata)
        matchProRata(order);
    else
        match(order);

    // --- Trigger stops / update pegs ---
    if (lastTradePrice_ > 0) {
        checkStopOrders(lastTradePrice_);
        updateTrailingStops(lastTradePrice_);
    }
    if (!peggedOrders_.empty())
        updatePeggedOrders();

    // --- Post-match: handle remaining quantity ---
    if (LIKELY(order->remainingQty > 0)) {
        if (type == OrderType::IOC || type == OrderType::FOK || type == OrderType::Market) {
            Quantity filled = order->initialQty - order->remainingQty;
            OrderStatus st = (filled > 0) ? OrderStatus::Cancelled : OrderStatus::Cancelled;
            notifyOrderUpdate(orderId, st, filled, 0);
            orderLookup_[orderId] = nullptr;
            orderPool_.deallocate(order);
        } else {
            // Rest in book (Limit, PostOnly, Hidden, Iceberg)
            if (type == OrderType::Iceberg)
                order->visibleQty = std::min(order->remainingQty, order->displayQty);
            if (!addToBook(order)) {
                // Depth limit exceeded — cancel the order
                Quantity filled = order->initialQty - order->remainingQty;
                notifyOrderUpdate(orderId, OrderStatus::Cancelled, filled, 0);
                orderLookup_[orderId] = nullptr;
                orderPool_.deallocate(order);
            } else {
                order->status = (order->remainingQty < order->initialQty)
                                ? OrderStatus::PartiallyFilled : OrderStatus::Accepted;
                if (!order->isHidden)
                    notifyMarketData(MarketDataUpdate::Action::Add, side, price);
            }
        }
    } else {
        order->status = OrderStatus::Filled;
        notifyOrderUpdate(orderId, OrderStatus::Filled, order->initialQty, 0, lastTradePrice_);
        orderLookup_[orderId] = nullptr;
        orderPool_.deallocate(order);
    }
}

// ─── Match (Price-Time FIFO) ─────────────────────────────────────────────────

void OrderBook::match(Order* incoming) {
    while (incoming->remainingQty > 0) {
        if (incoming->side == Side::Buy) {
            if (UNLIKELY(asks_.empty())) break;

            auto bestIt = asks_.begin();
            Price bestPrice = bestIt->first;

            if (LIKELY(incoming->type != OrderType::Market && incoming->price < bestPrice))
                break;

            OrderList& level = bestIt->second;
            Order* bookOrder = level.front();

            if (UNLIKELY(checkSMP(*incoming, *bookOrder))) {
                incoming->remainingQty = 0;
                break;
            }

            Quantity available = (bookOrder->type == OrderType::Iceberg)
                                 ? bookOrder->visibleQty : bookOrder->remainingQty;
            Quantity fillQty = std::min(incoming->remainingQty, available);

            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

            lastTradePrice_ = bestPrice;
            lastTradeQty_ = fillQty;
            updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);
            otrStats_[incoming->participantId].netPosition += static_cast<int64_t>(fillQty);
            otrStats_[bookOrder->participantId].netPosition -= static_cast<int64_t>(fillQty);

            Trade t{};
            t.tradeId = nextTradeId_++;
            t.buyOrderId = incoming->id;
            t.sellOrderId = bookOrder->id;
            t.buyerId = incoming->participantId;
            t.sellerId = bookOrder->participantId;
            t.price = bestPrice;
            t.quantity = fillQty;
            t.timestamp = nowNs();
            t.symbolId = symbolId_;
            tradeHistory_.push_back(t);
            if (onTrade_) onTrade_(t);

            if (UNLIKELY(bookOrder->remainingQty == 0)) {
                bookOrder->status = OrderStatus::Filled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
                level.remove(bookOrder);
                orderLookup_[bookOrder->id] = nullptr;
                orderPool_.deallocate(bookOrder);
                if (UNLIKELY(level.empty())) asks_.erase(bestIt);
            } else if (UNLIKELY(bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0)) {
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                bookOrder->status = OrderStatus::PartiallyFilled;
                level.remove(bookOrder);
                level.push_back(bookOrder);
            } else {
                bookOrder->status = OrderStatus::PartiallyFilled;
            }

        } else { // Sell side
            if (UNLIKELY(bids_.empty())) break;

            auto bestIt = bids_.begin();
            Price bestPrice = bestIt->first;

            if (LIKELY(incoming->type != OrderType::Market && incoming->price > bestPrice))
                break;

            OrderList& level = bestIt->second;
            Order* bookOrder = level.front();

            if (UNLIKELY(checkSMP(*incoming, *bookOrder))) {
                incoming->remainingQty = 0;
                break;
            }

            Quantity available = (bookOrder->type == OrderType::Iceberg)
                                 ? bookOrder->visibleQty : bookOrder->remainingQty;
            Quantity fillQty = std::min(incoming->remainingQty, available);

            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

            lastTradePrice_ = bestPrice;
            lastTradeQty_ = fillQty;
            updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);
            otrStats_[bookOrder->participantId].netPosition += static_cast<int64_t>(fillQty);
            otrStats_[incoming->participantId].netPosition -= static_cast<int64_t>(fillQty);

            Trade t{};
            t.tradeId = nextTradeId_++;
            t.buyOrderId = bookOrder->id;
            t.sellOrderId = incoming->id;
            t.buyerId = bookOrder->participantId;
            t.sellerId = incoming->participantId;
            t.price = bestPrice;
            t.quantity = fillQty;
            t.timestamp = nowNs();
            t.symbolId = symbolId_;
            tradeHistory_.push_back(t);
            if (onTrade_) onTrade_(t);

            if (UNLIKELY(bookOrder->remainingQty == 0)) {
                bookOrder->status = OrderStatus::Filled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
                level.remove(bookOrder);
                orderLookup_[bookOrder->id] = nullptr;
                orderPool_.deallocate(bookOrder);
                if (UNLIKELY(level.empty())) bids_.erase(bestIt);
            } else if (UNLIKELY(bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0)) {
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                bookOrder->status = OrderStatus::PartiallyFilled;
                level.remove(bookOrder);
                level.push_back(bookOrder);
            } else {
                bookOrder->status = OrderStatus::PartiallyFilled;
            }
        }
    }
}

// ─── Match (Pro-Rata) ────────────────────────────────────────────────────────

void OrderBook::matchProRata(Order* incoming) {
    while (incoming->remainingQty > 0) {
        bool isBuy = (incoming->side == Side::Buy);

        // Select opposite book
        Price bestPrice;
        OrderList* levelPtr;

        if (isBuy) {
            if (asks_.empty()) break;
            auto bestIt = asks_.begin();
            bestPrice = bestIt->first;
            if (incoming->type != OrderType::Market && incoming->price < bestPrice) break;
            levelPtr = &bestIt->second;
        } else {
            if (bids_.empty()) break;
            auto bestIt = bids_.begin();
            bestPrice = bestIt->first;
            if (incoming->type != OrderType::Market && incoming->price > bestPrice) break;
            levelPtr = &bestIt->second;
        }

        OrderList& level = *levelPtr;

        // Calculate total quantity at this level
        Quantity totalLevelQty = 0;
        for (Order* o = level.front(); o; o = o->next) {
            totalLevelQty += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
        }
        if (totalLevelQty == 0) {
            if (isBuy) asks_.erase(asks_.begin());
            else bids_.erase(bids_.begin());
            continue;
        }

        Quantity toFill = std::min(incoming->remainingQty, totalLevelQty);

        // Compute proportional allocations
        struct Alloc { Order* order; Quantity qty; };
        std::vector<Alloc> allocs;
        Quantity allocated = 0;

        for (Order* o = level.front(); o; o = o->next) {
            if (checkSMP(*incoming, *o)) {
                incoming->remainingQty = 0;
                return;
            }
            Quantity avail = (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
            Quantity share = (totalLevelQty > 0)
                ? static_cast<Quantity>(static_cast<double>(avail) / totalLevelQty * toFill)
                : 0;
            allocs.push_back({o, share});
            allocated += share;
        }

        // Distribute rounding remainder by time priority
        Quantity remainder = toFill - allocated;
        for (auto& a : allocs) {
            if (remainder == 0) break;
            Quantity avail = (a.order->type == OrderType::Iceberg) ? a.order->visibleQty : a.order->remainingQty;
            Quantity extra = std::min(remainder, avail - a.qty);
            a.qty += extra;
            remainder -= extra;
        }

        // Execute fills
        std::vector<Order*> toRemove;
        for (auto& a : allocs) {
            if (a.qty == 0) continue;
            Order* bookOrder = a.order;
            Quantity fillQty = a.qty;

            incoming->remainingQty -= fillQty;
            bookOrder->remainingQty -= fillQty;
            if (bookOrder->type == OrderType::Iceberg) bookOrder->visibleQty -= fillQty;

            lastTradePrice_ = bestPrice;
            lastTradeQty_ = fillQty;

            OrderId buyId = isBuy ? incoming->id : bookOrder->id;
            OrderId sellId = isBuy ? bookOrder->id : incoming->id;
            ParticipantId buyerId = isBuy ? incoming->participantId : bookOrder->participantId;
            ParticipantId sellerId = isBuy ? bookOrder->participantId : incoming->participantId;

            updateAnalytics(bestPrice, fillQty, bookOrder->participantId, incoming->participantId);
            otrStats_[buyerId].netPosition += static_cast<int64_t>(fillQty);
            otrStats_[sellerId].netPosition -= static_cast<int64_t>(fillQty);

            Trade t{};
            t.tradeId = nextTradeId_++;
            t.buyOrderId = buyId;
            t.sellOrderId = sellId;
            t.buyerId = buyerId;
            t.sellerId = sellerId;
            t.price = bestPrice;
            t.quantity = fillQty;
            t.timestamp = nowNs();
            t.symbolId = symbolId_;
            tradeHistory_.push_back(t);
            if (onTrade_) onTrade_(t);

            if (bookOrder->remainingQty == 0) {
                bookOrder->status = OrderStatus::Filled;
                notifyOrderUpdate(bookOrder->id, OrderStatus::Filled, bookOrder->initialQty, 0, bestPrice);
                toRemove.push_back(bookOrder);
            } else if (bookOrder->type == OrderType::Iceberg && bookOrder->visibleQty == 0) {
                bookOrder->visibleQty = std::min(bookOrder->remainingQty, bookOrder->displayQty);
                bookOrder->status = OrderStatus::PartiallyFilled;
            } else {
                bookOrder->status = OrderStatus::PartiallyFilled;
            }
        }

        for (Order* o : toRemove) {
            level.remove(o);
            orderLookup_[o->id] = nullptr;
            orderPool_.deallocate(o);
        }

        if (level.empty()) {
            if (isBuy) asks_.erase(asks_.begin());
            else bids_.erase(bids_.begin());
        }
    }
}

// ─── Cancel ──────────────────────────────────────────────────────────────────

void OrderBook::cancelOrder(OrderId orderId) {
    if (UNLIKELY(orderId >= orderLookup_.size() || orderLookup_[orderId] == nullptr))
        return;

    Order* order = orderLookup_[orderId];
    otrStats_[order->participantId].ordersSubmitted++;

    // Remove from special tracking lists
    if (order->type == OrderType::Stop || order->type == OrderType::StopLimit) {
        stopOrders_.erase(std::remove(stopOrders_.begin(), stopOrders_.end(), order), stopOrders_.end());
    } else if (order->type == OrderType::TrailingStop) {
        trailingStopOrders_.erase(std::remove(trailingStopOrders_.begin(), trailingStopOrders_.end(), order),
                                   trailingStopOrders_.end());
    } else if (order->type == OrderType::Pegged) {
        peggedOrders_.erase(std::remove(peggedOrders_.begin(), peggedOrders_.end(), order), peggedOrders_.end());
    }

    removeFromBook(order);

    if (!order->isHidden)
        notifyMarketData(MarketDataUpdate::Action::Delete, order->side, order->price);

    Quantity filledQty = order->initialQty - order->remainingQty;
    notifyOrderUpdate(orderId, OrderStatus::Cancelled, filledQty, 0);

    orderLookup_[orderId] = nullptr;
    orderPool_.deallocate(order);
}

// ─── Modify (quantity reduction only, preserves time priority) ───────────────

bool OrderBook::modifyOrder(OrderId orderId, Quantity newQty) {
    if (UNLIKELY(orderId >= orderLookup_.size() || orderLookup_[orderId] == nullptr))
        return false;

    Order* order = orderLookup_[orderId];

    if (newQty < order->remainingQty) {
        order->remainingQty = newQty;
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
        return true;
    }

    return false;
}

// ─── Cancel/Replace (full amendment, price change loses priority) ────────────

bool OrderBook::cancelReplace(OrderId orderId, Price newPrice, Quantity newQty) {
    if (UNLIKELY(orderId >= orderLookup_.size() || orderLookup_[orderId] == nullptr))
        return false;

    Order* order = orderLookup_[orderId];

    if (UNLIKELY(newQty == 0 || newPrice <= 0))
        return false;

    Price oldPrice = order->price;
    bool priceChanged = (newPrice != oldPrice);

    if (priceChanged) {
        removeFromBook(order);
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Delete, order->side, oldPrice);

        order->price = newPrice;
        order->remainingQty = newQty;
        order->timestamp = nowNs();

        // Check if new price crosses — if so, match first
        bool wouldCross = false;
        if (order->side == Side::Buy)
            wouldCross = !asks_.empty() && newPrice >= asks_.begin()->first;
        else
            wouldCross = !bids_.empty() && newPrice <= bids_.begin()->first;

        if (wouldCross) {
            if (matchAlgorithm_ == MatchAlgorithm::ProRata)
                matchProRata(order);
            else
                match(order);
        }

        if (order->remainingQty > 0) {
            addToBook(order);
            if (!order->isHidden)
                notifyMarketData(MarketDataUpdate::Action::Add, order->side, newPrice);
        } else {
            order->status = OrderStatus::Filled;
            notifyOrderUpdate(orderId, OrderStatus::Filled, order->initialQty, 0, lastTradePrice_);
            orderLookup_[orderId] = nullptr;
            orderPool_.deallocate(order);
        }
    } else {
        // Same price
        if (newQty < order->remainingQty) {
            order->remainingQty = newQty;
        } else {
            // Quantity increase loses time priority
            removeFromBook(order);
            order->remainingQty = newQty;
            order->timestamp = nowNs();
            addToBook(order);
        }
        if (!order->isHidden)
            notifyMarketData(MarketDataUpdate::Action::Modify, order->side, order->price);
    }

    return true;
}

// ─── Kill Switch ─────────────────────────────────────────────────────────────

uint64_t OrderBook::cancelAllForParticipant(ParticipantId participantId) {
    // Collect IDs first to avoid iterator invalidation
    std::vector<OrderId> toCancel;
    for (size_t i = 0; i < orderLookup_.size(); ++i) {
        Order* order = orderLookup_[i];
        if (order && order->participantId == participantId)
            toCancel.push_back(order->id);
    }

    for (OrderId id : toCancel)
        cancelOrder(id);

    return toCancel.size();
}

// ─── Time-based expiry ───────────────────────────────────────────────────────

void OrderBook::expireOrders(uint64_t currentTime) {
    std::vector<OrderId> toExpire;
    for (size_t i = 0; i < orderLookup_.size(); ++i) {
        Order* order = orderLookup_[i];
        if (!order) continue;

        if (order->timeInForce == TimeInForce::GTD && currentTime >= order->expiryTime)
            toExpire.push_back(order->id);
        else if (order->timeInForce == TimeInForce::DAY && currentTime >= order->expiryTime)
            toExpire.push_back(order->id);
    }

    for (OrderId id : toExpire)
        cancelOrder(id);
}

// ─── Stop Order Triggers ─────────────────────────────────────────────────────

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
            order->isStopTriggered = true;

            if (order->type == OrderType::StopLimit) {
                order->type = OrderType::Limit;
                order->price = order->stopLimitPrice;
            } else {
                order->type = OrderType::Limit;
                // price is already set (limit price = stop price for regular stops)
            }

            it = stopOrders_.erase(it);
            match(order);

            if (order->remainingQty > 0) {
                addToBook(order);
            } else {
                order->status = OrderStatus::Filled;
                orderLookup_[order->id] = nullptr;
                orderPool_.deallocate(order);
            }
        } else {
            ++it;
        }
    }
}

// ─── Trailing Stop Updates ───────────────────────────────────────────────────

void OrderBook::updateTrailingStops(Price lastTradePrice) {
    if (UNLIKELY(trailingStopOrders_.empty())) return;

    for (auto it = trailingStopOrders_.begin(); it != trailingStopOrders_.end(); ) {
        Order* order = *it;
        bool triggered = false;

        if (order->side == Side::Buy) {
            // Buy trailing: track lowest price, trigger when price rises to stop
            if (lastTradePrice < order->trailRefPrice) {
                order->trailRefPrice = lastTradePrice;
                order->stopPrice = order->trailRefPrice + order->trailAmount;
            }
            if (lastTradePrice >= order->stopPrice) triggered = true;
        } else {
            // Sell trailing: track highest price, trigger when price drops to stop
            if (lastTradePrice > order->trailRefPrice) {
                order->trailRefPrice = lastTradePrice;
                order->stopPrice = order->trailRefPrice - order->trailAmount;
            }
            if (lastTradePrice <= order->stopPrice) triggered = true;
        }

        if (triggered) {
            order->type = OrderType::Limit;
            order->price = order->stopPrice;
            it = trailingStopOrders_.erase(it);
            match(order);

            if (order->remainingQty > 0) {
                addToBook(order);
            } else {
                order->status = OrderStatus::Filled;
                orderLookup_[order->id] = nullptr;
                orderPool_.deallocate(order);
            }
        } else {
            ++it;
        }
    }
}

// ─── Pegged Order Re-pricing ─────────────────────────────────────────────────

void OrderBook::updatePeggedOrders() {
    for (auto it = peggedOrders_.begin(); it != peggedOrders_.end(); ) {
        Order* order = *it;

        // Check if order was removed
        if (order->id >= orderLookup_.size() || orderLookup_[order->id] != order) {
            it = peggedOrders_.erase(it);
            continue;
        }

        Price newPrice = order->price;
        if (order->pegType == PegType::MidPeg) {
            Price mid = getMidPrice();
            if (mid > 0) newPrice = mid + order->pegOffset;
        } else if (order->pegType == PegType::PrimaryPeg) {
            if (order->side == Side::Buy) {
                Price bb = getBestBid();
                if (bb > 0) newPrice = bb + order->pegOffset;
            } else {
                Price ba = getBestAsk();
                if (ba < std::numeric_limits<Price>::max()) newPrice = ba + order->pegOffset;
            }
        }

        if (newPrice != order->price && newPrice > 0) {
            removeFromBook(order);
            order->price = newPrice;
            order->timestamp = nowNs();
            addToBook(order);
        }

        ++it;
    }
}

// ─── Analytics ───────────────────────────────────────────────────────────────

void OrderBook::updateAnalytics(Price price, Quantity qty, ParticipantId p1, ParticipantId p2) {
    double tradeValue = static_cast<double>(price) * qty;
    vwap_ = (vwap_ * totalQty_ + tradeValue) / (totalQty_ + qty);
    totalQty_ += qty;

    cumulativePrice_ += price;
    priceUpdates_++;

    if (p1 > 0) otrStats_[p1].tradesExecuted++;
    if (p2 > 0) otrStats_[p2].tradesExecuted++;
}

// ─── Uncross (Auction) ──────────────────────────────────────────────────────

void OrderBook::uncross() {
    if (bids_.empty() || asks_.empty()) return;

    // Step 1: Discover the uncross price that maximizes matched volume
    Price bestUncrossPrice = 0;
    Quantity maxVolume = 0;

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
                for (Order* o = list.front(); o; o = o->next) cumBuy += o->remainingQty;
            }
        }
        for (const auto& [ap, list] : asks_) {
            if (ap <= p) {
                for (Order* o = list.front(); o; o = o->next) cumSell += o->remainingQty;
            }
        }

        Quantity volume = std::min(cumBuy, cumSell);
        if (volume > maxVolume) {
            maxVolume = volume;
            bestUncrossPrice = p;
        }
    }

    if (maxVolume == 0) return;

    // Step 2: Execute trades at the uncross price
    // Walk eligible bids (price >= uncross) and asks (price <= uncross),
    // matching by price-time priority, all at the single uncross price.
    Quantity remainingVolume = maxVolume;

    while (remainingVolume > 0) {
        // Find best eligible bid
        if (bids_.empty()) break;
        auto bidIt = bids_.begin();
        if (bidIt->first < bestUncrossPrice) break;

        // Find best eligible ask
        if (asks_.empty()) break;
        auto askIt = asks_.begin();
        if (askIt->first > bestUncrossPrice) break;

        OrderList& bidLevel = bidIt->second;
        OrderList& askLevel = askIt->second;

        Order* buyer = bidLevel.front();
        Order* seller = askLevel.front();

        if (!buyer || !seller) break;

        // SMP check — skip this pair in auction
        if (checkSMP(*buyer, *seller)) {
            // Cancel the buyer (arbitrary choice for auction SMP)
            bidLevel.remove(buyer);
            notifyOrderUpdate(buyer->id, OrderStatus::Cancelled, 0, buyer->remainingQty);
            orderLookup_[buyer->id] = nullptr;
            orderPool_.deallocate(buyer);
            if (bidLevel.empty()) bids_.erase(bidIt);
            continue;
        }

        Quantity fillQty = std::min({buyer->remainingQty, seller->remainingQty, remainingVolume});

        buyer->remainingQty -= fillQty;
        seller->remainingQty -= fillQty;
        remainingVolume -= fillQty;

        lastTradePrice_ = bestUncrossPrice;
        lastTradeQty_ = fillQty;
        updateAnalytics(bestUncrossPrice, fillQty, buyer->participantId, seller->participantId);
        otrStats_[buyer->participantId].netPosition += static_cast<int64_t>(fillQty);
        otrStats_[seller->participantId].netPosition -= static_cast<int64_t>(fillQty);

        Trade t{};
        t.tradeId = nextTradeId_++;
        t.buyOrderId = buyer->id;
        t.sellOrderId = seller->id;
        t.buyerId = buyer->participantId;
        t.sellerId = seller->participantId;
        t.price = bestUncrossPrice;
        t.quantity = fillQty;
        t.timestamp = nowNs();
        t.symbolId = symbolId_;
        tradeHistory_.push_back(t);
        if (onTrade_) onTrade_(t);

        // Remove filled orders
        if (buyer->remainingQty == 0) {
            buyer->status = OrderStatus::Filled;
            notifyOrderUpdate(buyer->id, OrderStatus::Filled, buyer->initialQty, 0, bestUncrossPrice);
            bidLevel.remove(buyer);
            orderLookup_[buyer->id] = nullptr;
            orderPool_.deallocate(buyer);
            if (bidLevel.empty()) bids_.erase(bidIt);
        }

        if (seller->remainingQty == 0) {
            seller->status = OrderStatus::Filled;
            notifyOrderUpdate(seller->id, OrderStatus::Filled, seller->initialQty, 0, bestUncrossPrice);
            askLevel.remove(seller);
            orderLookup_[seller->id] = nullptr;
            orderPool_.deallocate(seller);
            if (askLevel.empty()) asks_.erase(askIt);
        }
    }
}

// ─── Market Data Snapshot ────────────────────────────────────────────────────

MarketDataSnapshot OrderBook::getSnapshot(size_t depth) const {
    MarketDataSnapshot snap{};
    snap.symbolId = symbolId_;
    snap.lastTradePrice = lastTradePrice_;
    snap.lastTradeQty = lastTradeQty_;
    snap.timestamp = nowNs();

    size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (count >= depth) break;
        PriceLevel pl{};
        pl.price = price;
        for (Order* o = level.front(); o; o = o->next) {
            if (!o->isHidden) {
                pl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                pl.orderCount++;
            }
        }
        if (pl.orderCount > 0) {
            snap.bids.push_back(pl);
            count++;
        }
    }

    count = 0;
    for (const auto& [price, level] : asks_) {
        if (count >= depth) break;
        PriceLevel pl{};
        pl.price = price;
        for (Order* o = level.front(); o; o = o->next) {
            if (!o->isHidden) {
                pl.totalQuantity += (o->type == OrderType::Iceberg) ? o->visibleQty : o->remainingQty;
                pl.orderCount++;
            }
        }
        if (pl.orderCount > 0) {
            snap.asks.push_back(pl);
            count++;
        }
    }

    return snap;
}

// ─── All Orders (for snapshots) ──────────────────────────────────────────────

std::vector<const Order*> OrderBook::getAllOrders() const {
    std::vector<const Order*> orders;
    for (const auto* o : orderLookup_) {
        if (o) orders.push_back(o);
    }
    return orders;
}

// ─── Getters ─────────────────────────────────────────────────────────────────

const Order* OrderBook::getOrder(OrderId orderId) const {
    if (orderId < orderLookup_.size()) return orderLookup_[orderId];
    return nullptr;
}

size_t OrderBook::getBidLevelsCount() const { return bids_.size(); }
size_t OrderBook::getAskLevelsCount() const { return asks_.size(); }

Price OrderBook::getBestBid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::getBestAsk() const {
    return asks_.empty() ? std::numeric_limits<Price>::max() : asks_.begin()->first;
}

Price OrderBook::getMidPrice() const {
    Price bb = getBestBid();
    Price ba = getBestAsk();
    if (bb == 0 || ba == std::numeric_limits<Price>::max()) return 0;
    return (bb + ba) / 2;
}

} // namespace OrderMatcher
