#include <gtest/gtest.h>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "FIXParser.h"
#include "LatencyTracker.h"
#include "Types.h"
#include <cstdio>

using namespace OrderMatcher;

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;

    void SetUp() override {
        // Common setup
    }
};

TEST_F(OrderBookTest, AddOrder_RetainsInBook) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    const Order* order = book.getOrder(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remainingQty, 100);
    EXPECT_EQ(book.getBidLevelsCount(), 1);
}

TEST_F(OrderBookTest, ExecuteMatch_FullFill) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, ExecuteMatch_PartialFill) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::Limit);

    const Order* order1 = book.getOrder(1);
    ASSERT_NE(order1, nullptr);
    EXPECT_EQ(order1->remainingQty, 50);
    EXPECT_EQ(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, CancelOrder) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.cancelOrder(1);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, CancelReplace_PriceChange) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    bool ok = book.cancelReplace(1, 1010000, 80);

    EXPECT_TRUE(ok);
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->price, 1010000);
    EXPECT_EQ(o->remainingQty, 80);
}

TEST_F(OrderBookTest, PostOnly_Reject) {
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);

    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getOrder(1)->remainingQty, 100);
}

TEST_F(OrderBookTest, PostOnly_Accept) {
    book.addOrder(1, 1, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 50, OrderType::PostOnly);

    EXPECT_NE(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 1);
}

TEST_F(OrderBookTest, HiddenOrder_NotInSnapshot) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Hidden);
    auto snap = book.getSnapshot();
    EXPECT_TRUE(snap.bids.empty());
}

TEST_F(OrderBookTest, KillSwitch) {
    book.addOrder(1, 1, Side::Buy, 990000, 100, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 1010000, 100, OrderType::Limit);
    book.addOrder(3, 2, Side::Buy, 980000, 100, OrderType::Limit);

    uint64_t cancelled = book.cancelAllForParticipant(1);
    EXPECT_EQ(cancelled, 2);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_NE(book.getOrder(3), nullptr);
}

// ─── Circuit Breaker Tests ──────────────────────────────────────────────────

TEST_F(OrderBookTest, CircuitBreaker_ConfigurableThreshold) {
    book.setCircuitBreakerThreshold(0.02); // 2% threshold
    EXPECT_DOUBLE_EQ(book.getCircuitBreakerThreshold(), 0.02);

    // Set reference price at 100.0000
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);

    // 1.5% move should be OK
    book.addOrder(2, 2, Side::Sell, 1015000, 100, OrderType::Limit);
    EXPECT_FALSE(book.isHalted());

    // 3% move should trigger halt
    book.addOrder(3, 3, Side::Sell, 1030000, 100, OrderType::Limit);
    EXPECT_TRUE(book.isHalted());
}

TEST_F(OrderBookTest, CircuitBreaker_DefaultThreshold) {
    // Default is 5%
    EXPECT_DOUBLE_EQ(book.getCircuitBreakerThreshold(), 0.05);
}

// ─── Modify Order Tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, ModifyOrder_ReduceQuantity) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    bool ok = book.modifyOrder(1, 50);

    EXPECT_TRUE(ok);
    const Order* o = book.getOrder(1);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->remainingQty, 50);
}

// ─── IOC Order Tests ────────────────────────────────────────────────────────

TEST_F(OrderBookTest, IOC_PartialFillCancelsRest) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::IOC);

    // IOC should fill 50, cancel remaining 50
    EXPECT_EQ(book.getOrder(1), nullptr); // Sell fully filled
    EXPECT_EQ(book.getOrder(2), nullptr); // IOC cancelled after partial fill
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, IOC_NoLiquidity_Cancelled) {
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::IOC);
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

// ─── FOK Order Tests ────────────────────────────────────────────────────────

TEST_F(OrderBookTest, FOK_InsufficientLiquidity_Rejected) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::FOK);

    // FOK needs 100, only 50 available — rejected
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_NE(book.getOrder(1), nullptr); // Sell untouched
    EXPECT_EQ(book.getOrder(1)->remainingQty, 50);
}

// ─── Market Order Tests ─────────────────────────────────────────────────────

TEST_F(OrderBookTest, MarketOrder_SweepsMultipleLevels) {
    book.addOrder(1, 1, Side::Sell, 1000000, 50, OrderType::Limit);
    book.addOrder(2, 1, Side::Sell, 1010000, 50, OrderType::Limit);
    book.addOrder(3, 2, Side::Buy, 0, 100, OrderType::Market);

    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getOrder(3), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
}

// ─── FIX Parser Tests ───────────────────────────────────────────────────────

TEST(FIXParserTest, ParseAndBuildRoundTrip) {
    FIXMessage msg;
    msg.setField(FIXTag::MsgType, FIXMsgType::NewOrderSingle);
    msg.setField(FIXTag::ClOrdID, int64_t(42));
    msg.setField(FIXTag::Account, int64_t(1));
    msg.setField(FIXTag::Symbol, int64_t(100));
    msg.setField(FIXTag::Side, int64_t(1)); // Buy
    msg.setField(FIXTag::Price, 150.5000);
    msg.setField(FIXTag::OrderQty, int64_t(200));
    msg.setField(FIXTag::OrdType, std::string("2")); // Limit

    std::string raw = msg.build();
    FIXMessage parsed = FIXMessage::parse(raw);

    EXPECT_EQ(parsed.getField(FIXTag::MsgType), "D");
    EXPECT_EQ(parsed.getInt(FIXTag::ClOrdID), 42);
    EXPECT_EQ(parsed.getInt(FIXTag::OrderQty), 200);
}

TEST(FIXParserTest, ParseNewOrder) {
    std::string raw = "35=D|11=1001|1=5|55=0|54=1|44=100.0000|38=50|40=2|59=1|";
    FIXMessage msg = FIXMessage::parse(raw);
    auto params = FIXAdapter::parseNewOrder(msg);

    EXPECT_EQ(params.orderId, 1001);
    EXPECT_EQ(params.participantId, 5);
    EXPECT_EQ(params.side, Side::Buy);
    EXPECT_EQ(params.qty, 50);
    EXPECT_EQ(params.type, OrderType::Limit);
}

// ─── FIX Gateway Integration Test ──────────────────────────────────────────

TEST(MatchingEngineTest, FIXGateway_NewOrderAndCancel) {
    MatchingEngine engine;
    engine.start();
    engine.addSymbol(100);

    // Submit a new order via FIX
    std::string newOrder = "35=D|11=1|1=1|55=100|54=1|44=100.0000|38=50|40=2|59=1|";
    engine.processFIXMessage(newOrder);

    auto* book = engine.getOrderBook(100);
    ASSERT_NE(book, nullptr);
    EXPECT_NE(book->getOrder(1), nullptr);

    // Cancel via FIX
    std::string cancelOrder = "35=F|11=1|55=100|";
    engine.processFIXMessage(cancelOrder);
    EXPECT_EQ(book->getOrder(1), nullptr);

    engine.stop();
}

// ─── Journal Replay Test ────────────────────────────────────────────────────

TEST(JournalTest, ReplayRecovery) {
    const char* journalPath = "/tmp/test_journal_replay.bin";

    // Phase 1: Write some orders to journal
    {
        MatchingEngine engine;
        engine.start();
        engine.addSymbol(1);
        engine.enableJournal(journalPath);

        engine.processOrder(1, 100, 1, Side::Buy, 1000000, 50, OrderType::Limit);
        engine.processOrder(1, 101, 2, Side::Sell, 1010000, 30, OrderType::Limit);

        engine.stop();
    }

    // Phase 2: Recover from journal
    {
        MatchingEngine engine;
        engine.start();
        engine.addSymbol(1);
        engine.enableJournal(journalPath);

        size_t replayed = engine.replayJournal();
        EXPECT_EQ(replayed, 2);

        auto* book = engine.getOrderBook(1);
        ASSERT_NE(book, nullptr);
        // Orders should be reconstructed (they won't match since they don't cross)
        EXPECT_NE(book->getOrder(100), nullptr);
        EXPECT_NE(book->getOrder(101), nullptr);

        engine.stop();
    }

    std::remove(journalPath);
}

// ─── Latency Tracker Tests ──────────────────────────────────────────────────

TEST(LatencyTrackerTest, BasicRecording) {
    LatencyTracker tracker;

    for (int i = 1; i <= 100; i++) {
        tracker.record(static_cast<uint64_t>(i));
    }

    EXPECT_EQ(tracker.getCount(), 100);
    EXPECT_EQ(tracker.getMin(), 1);
    EXPECT_EQ(tracker.getMax(), 100);
    EXPECT_NEAR(tracker.getMean(), 50.5, 0.01);
}

TEST(LatencyTrackerTest, Percentiles) {
    LatencyTracker tracker;

    // Record 1000 values from 1 to 1000
    for (int i = 1; i <= 1000; i++) {
        tracker.record(static_cast<uint64_t>(i));
    }

    // P50 should be approximately 500
    EXPECT_NEAR(tracker.getP50(), 500, 10);
    // P99 should be approximately 990
    EXPECT_NEAR(tracker.getP99(), 990, 15);
}

TEST(LatencyTrackerTest, Reset) {
    LatencyTracker tracker;
    tracker.record(100);
    tracker.record(200);
    tracker.reset();

    EXPECT_EQ(tracker.getCount(), 0);
    EXPECT_NEAR(tracker.getMean(), 0.0, 0.01);
}

TEST(LatencyTrackerTest, ScopeTimer) {
    LatencyTracker tracker;

    {
        auto timer = tracker.scope();
        // Do some trivial work
        volatile int sum = 0;
        for (int i = 0; i < 100; i++) sum += i;
    }

    EXPECT_EQ(tracker.getCount(), 1);
    EXPECT_GT(tracker.getMax(), 0);
}
