#include "MatchingEngine.h"
#include <iostream>

namespace OrderMatcher {

MatchingEngine::MatchingEngine() {
    ensureDefaultSymbol();
}

MatchingEngine::~MatchingEngine() {
    if (async_)
        stopAsync();
    else
        stop();
}

void MatchingEngine::ensureDefaultSymbol() {
    if (books_.find(0) == books_.end()) {
        books_[0] = std::make_unique<OrderBook>(0);
    }
}

// ─── Sync mode ──────────────────────────────────────────────────────────────

void MatchingEngine::start() {
    running_ = true;
}

void MatchingEngine::stop() {
    running_ = false;
}

// ─── Async mode ─────────────────────────────────────────────────────────────

void MatchingEngine::startAsync(int coreId, size_t queueSize) {
    if (async_) return; // already running

    requestQueue_ = std::make_unique<RingBuffer<OrderRequest>>(queueSize);
    running_ = true;
    async_ = true;
    submitted_ = 0;
    processed_ = 0;

    workerThread_ = std::thread(&MatchingEngine::workerLoop, this, coreId);
}

void MatchingEngine::stopAsync() {
    if (!async_) return;

    // Send shutdown message
    OrderRequest shutdown{};
    shutdown.type = OrderRequest::Type::Shutdown;
    while (!requestQueue_->push(shutdown)) {
        std::this_thread::yield();
    }
    submitted_.fetch_add(1, std::memory_order_relaxed);

    if (workerThread_.joinable())
        workerThread_.join();

    running_ = false;
    async_ = false;
    requestQueue_.reset();
}

void MatchingEngine::waitForDrain() {
    if (!async_) return;
    while (processed_.load(std::memory_order_acquire) < submitted_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void MatchingEngine::workerLoop(int coreId) {
    // Pin thread to core if requested
    if (coreId >= 0) {
        Utils::pinThread(coreId);
    }

    OrderRequest req;
    while (true) {
        if (requestQueue_->pop(req)) {
            if (req.type == OrderRequest::Type::Shutdown) {
                processed_.fetch_add(1, std::memory_order_release);
                break;
            }
            processRequest(req);
            processed_.fetch_add(1, std::memory_order_release);
        }
        // Busy-wait for ultra-low latency (no sleep/yield on hot path)
    }
}

void MatchingEngine::processRequest(const OrderRequest& req) {
    switch (req.type) {
        case OrderRequest::Type::NewOrder: {
            auto* book = getOrderBook(req.symbolId);
            if (!book) return;
            if (journal_) {
                journal_->logAddOrder(req.orderId, req.participantId, req.symbolId,
                                       req.side, req.price, req.qty, req.orderType,
                                       req.tif, req.expiryTime, req.stopPrice,
                                       req.stopLimitPrice, req.displayQty, req.pegType,
                                       req.pegOffset, req.trailAmount, req.minQty, req.hidden);
            }
            book->addOrder(req.orderId, req.participantId, req.side, req.price, req.qty,
                           req.orderType, req.stopPrice, req.displayQty, req.tif, req.expiryTime,
                           req.stopLimitPrice, req.pegType, req.pegOffset, req.trailAmount,
                           req.minQty, req.hidden);
            break;
        }
        case OrderRequest::Type::Cancel: {
            auto* book = getOrderBook(req.symbolId);
            if (!book) return;
            if (journal_) journal_->logCancelOrder(req.orderId);
            book->cancelOrder(req.orderId);
            break;
        }
        case OrderRequest::Type::Modify: {
            auto* book = getOrderBook(req.symbolId);
            if (!book) return;
            if (journal_) journal_->logModifyOrder(req.orderId, req.newQty);
            book->modifyOrder(req.orderId, req.newQty);
            break;
        }
        case OrderRequest::Type::CancelReplace: {
            auto* book = getOrderBook(req.symbolId);
            if (!book) return;
            if (journal_) journal_->logCancelReplace(req.orderId, req.newPrice, req.newQty);
            book->cancelReplace(req.orderId, req.newPrice, req.newQty);
            break;
        }
        case OrderRequest::Type::KillSwitch: {
            for (auto& [_, book] : books_)
                book->cancelAllForParticipant(req.participantId);
            break;
        }
        default:
            break;
    }
}

// ─── Symbol management ──────────────────────────────────────────────────────

void MatchingEngine::addSymbol(SymbolId symbolId, MatchAlgorithm algo) {
    if (books_.find(symbolId) == books_.end()) {
        books_[symbolId] = std::make_unique<OrderBook>(symbolId, algo);
    }
}

OrderBook* MatchingEngine::getOrderBook(SymbolId symbolId) {
    auto it = books_.find(symbolId);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

const OrderBook* MatchingEngine::getOrderBook(SymbolId symbolId) const {
    auto it = books_.find(symbolId);
    return (it != books_.end()) ? it->second.get() : nullptr;
}

// ─── Multi-symbol gateway ───────────────────────────────────────────────────

void MatchingEngine::processOrder(SymbolId symbolId, OrderId orderId, ParticipantId participantId,
                                   Side side, Price price, Quantity qty, OrderType type,
                                   Price stopPrice, Quantity displayQty,
                                   TimeInForce tif, uint64_t expiryTime,
                                   Price stopLimitPrice,
                                   PegType pegType, Price pegOffset,
                                   Price trailAmount, Quantity minQty, bool hidden) {
    if (!running_) return;

    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::NewOrder;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.participantId = participantId;
        req.side = side;
        req.price = price;
        req.qty = qty;
        req.orderType = type;
        req.stopPrice = stopPrice;
        req.displayQty = displayQty;
        req.tif = tif;
        req.expiryTime = expiryTime;
        req.stopLimitPrice = stopLimitPrice;
        req.pegType = pegType;
        req.pegOffset = pegOffset;
        req.trailAmount = trailAmount;
        req.minQty = minQty;
        req.hidden = hidden;
        while (!requestQueue_->push(req)) { std::this_thread::yield(); }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Sync mode
    auto* book = getOrderBook(symbolId);
    if (!book) return;

    if (journal_) {
        journal_->logAddOrder(orderId, participantId, symbolId, side, price, qty, type,
                              tif, expiryTime, stopPrice, stopLimitPrice, displayQty,
                              pegType, pegOffset, trailAmount, minQty, hidden);
    }

    book->addOrder(orderId, participantId, side, price, qty, type,
                   stopPrice, displayQty, tif, expiryTime, stopLimitPrice,
                   pegType, pegOffset, trailAmount, minQty, hidden);
}

void MatchingEngine::cancelOrder(SymbolId symbolId, OrderId orderId) {
    if (!running_) return;

    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::Cancel;
        req.symbolId = symbolId;
        req.orderId = orderId;
        while (!requestQueue_->push(req)) { std::this_thread::yield(); }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto* book = getOrderBook(symbolId);
    if (!book) return;
    if (journal_) journal_->logCancelOrder(orderId);
    book->cancelOrder(orderId);
}

bool MatchingEngine::modifyOrder(SymbolId symbolId, OrderId orderId, Quantity newQty) {
    if (!running_) return false;

    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::Modify;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.newQty = newQty;
        while (!requestQueue_->push(req)) { std::this_thread::yield(); }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return true; // async — can't return result synchronously
    }

    auto* book = getOrderBook(symbolId);
    if (!book) return false;
    if (journal_) journal_->logModifyOrder(orderId, newQty);
    return book->modifyOrder(orderId, newQty);
}

bool MatchingEngine::cancelReplace(SymbolId symbolId, OrderId orderId, Price newPrice, Quantity newQty) {
    if (!running_) return false;

    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::CancelReplace;
        req.symbolId = symbolId;
        req.orderId = orderId;
        req.newPrice = newPrice;
        req.newQty = newQty;
        while (!requestQueue_->push(req)) { std::this_thread::yield(); }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto* book = getOrderBook(symbolId);
    if (!book) return false;
    if (journal_) journal_->logCancelReplace(orderId, newPrice, newQty);
    return book->cancelReplace(orderId, newPrice, newQty);
}

// ─── Kill switch ────────────────────────────────────────────────────────────

uint64_t MatchingEngine::killSwitch(ParticipantId participantId) {
    if (async_) {
        OrderRequest req{};
        req.type = OrderRequest::Type::KillSwitch;
        req.participantId = participantId;
        while (!requestQueue_->push(req)) { std::this_thread::yield(); }
        submitted_.fetch_add(1, std::memory_order_relaxed);
        waitForDrain();
    }

    uint64_t total = 0;
    if (!async_) {
        for (auto& [_, book] : books_)
            total += book->cancelAllForParticipant(participantId);
    }
    return total;
}

// ─── Risk ───────────────────────────────────────────────────────────────────

void MatchingEngine::setRiskLimits(SymbolId symbolId, ParticipantId participantId, const RiskLimits& limits) {
    auto* book = getOrderBook(symbolId);
    if (book) book->setRiskLimits(participantId, limits);
}

// ─── Market data ────────────────────────────────────────────────────────────

MarketDataSnapshot MatchingEngine::getSnapshot(SymbolId symbolId, size_t depth) {
    auto* book = getOrderBook(symbolId);
    if (book) return book->getSnapshot(depth);
    return {};
}

// ─── Auction ────────────────────────────────────────────────────────────────

void MatchingEngine::uncross(SymbolId symbolId) {
    auto* book = getOrderBook(symbolId);
    if (book) book->uncross();
}

// ─── Time management ────────────────────────────────────────────────────────

void MatchingEngine::expireOrders(uint64_t currentTime) {
    for (auto& [_, book] : books_) {
        book->expireOrders(currentTime);
    }
}

// ─── Journal ────────────────────────────────────────────────────────────────

void MatchingEngine::enableJournal(const std::string& path) {
    journal_ = std::make_unique<Journal>(path);
}

// ─── Journal replay/recovery ─────────────────────────────────────────────────

size_t MatchingEngine::replayJournal() {
    if (!journal_) return 0;

    auto entries = journal_->readAll(true);
    size_t replayed = 0;

    for (const auto& entry : entries) {
        switch (entry.entryType) {
            case JournalEntry::Type::AddOrder: {
                auto* book = getOrderBook(entry.symbolId);
                if (!book) {
                    addSymbol(entry.symbolId);
                    book = getOrderBook(entry.symbolId);
                }
                if (book) {
                    book->addOrder(entry.orderId, entry.participantId, entry.side,
                                   entry.price, entry.quantity, entry.orderType,
                                   entry.stopPrice, entry.displayQty, entry.timeInForce,
                                   entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                                   entry.pegOffset, entry.trailAmount, entry.minQty, entry.hidden);
                }
                break;
            }
            case JournalEntry::Type::CancelOrder: {
                for (auto& [_, book] : books_) {
                    const auto* order = book->getOrder(entry.orderId);
                    if (order) {
                        book->cancelOrder(entry.orderId);
                        break;
                    }
                }
                break;
            }
            case JournalEntry::Type::ModifyOrder: {
                for (auto& [_, book] : books_) {
                    const auto* order = book->getOrder(entry.orderId);
                    if (order) {
                        book->modifyOrder(entry.orderId, entry.newQty);
                        break;
                    }
                }
                break;
            }
            case JournalEntry::Type::CancelReplace: {
                for (auto& [_, book] : books_) {
                    const auto* order = book->getOrder(entry.orderId);
                    if (order) {
                        book->cancelReplace(entry.orderId, entry.newPrice, entry.newQty);
                        break;
                    }
                }
                break;
            }
            case JournalEntry::Type::Snapshot: {
                auto* book = getOrderBook(entry.symbolId);
                if (!book) {
                    addSymbol(entry.symbolId);
                    book = getOrderBook(entry.symbolId);
                }
                if (book) {
                    book->addOrder(entry.orderId, entry.participantId, entry.side,
                                   entry.price, entry.quantity, entry.orderType,
                                   entry.stopPrice, entry.displayQty, entry.timeInForce,
                                   entry.expiryTime, entry.stopLimitPrice, entry.pegType,
                                   entry.pegOffset, entry.trailAmount, entry.minQty, entry.hidden);
                }
                break;
            }
        }
        replayed++;
    }

    return replayed;
}

void MatchingEngine::checkpoint() {
    if (!journal_) return;

    // Truncate old journal entries
    journal_->truncate();

    // Write snapshot of all active orders across all books
    for (const auto& [symbolId, book] : books_) {
        auto orders = book->getAllOrders();
        for (const auto* order : orders) {
            journal_->logSnapshot(order->id, order->participantId, symbolId,
                                  order->side, order->price, order->remainingQty,
                                  order->type, order->timeInForce, order->expiryTime,
                                  order->stopPrice, order->stopLimitPrice, order->displayQty,
                                  order->pegType, order->pegOffset, order->trailAmount,
                                  order->minQty, order->isHidden);
        }
    }

    journal_->flush();
}

// ─── FIX protocol gateway ───────────────────────────────────────────────────

void MatchingEngine::processFIXMessage(const std::string& rawFix) {
    FIXMessage msg = FIXMessage::parse(rawFix);
    std::string msgType = msg.getField(FIXTag::MsgType);

    if (msgType == FIXMsgType::NewOrderSingle) {
        auto params = FIXAdapter::parseNewOrder(msg);
        processOrder(params.symbolId, params.orderId, params.participantId,
                     params.side, params.price, params.qty, params.type,
                     params.stopPrice, params.displayQty, params.tif, params.expiryTime);
    } else if (msgType == FIXMsgType::OrderCancelRequest) {
        auto params = FIXAdapter::parseCancelRequest(msg);
        cancelOrder(params.symbolId, params.orderId);
    } else if (msgType == FIXMsgType::OrderCancelReplace) {
        auto params = FIXAdapter::parseCancelReplace(msg);
        cancelReplace(params.symbolId, params.origOrderId, params.newPrice, params.newQty);
    }
}

// ─── Legacy single-symbol interface ─────────────────────────────────────────

void MatchingEngine::processOrder(OrderId orderId, ParticipantId participantId, Side side,
                                   Price price, Quantity qty, OrderType type,
                                   Price stopPrice, Quantity displayQty) {
    processOrder(static_cast<SymbolId>(0), orderId, participantId, side, price, qty, type, stopPrice, displayQty);
}

void MatchingEngine::cancelOrder(OrderId orderId) {
    cancelOrder(static_cast<SymbolId>(0), orderId);
}

double MatchingEngine::getVWAP() const {
    auto* book = getOrderBook(0);
    return book ? book->getVWAP() : 0.0;
}

} // namespace OrderMatcher
