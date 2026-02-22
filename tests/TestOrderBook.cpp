#include <gtest/gtest.h>
#include "OrderBook.h"
#include "Types.h"

using namespace OrderMatcher;

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book;
    
    void SetUp() override {
        // Common setup
    }
};

TEST_F(OrderBookTest, AddOrder_RetainsInBook) {
    // Buy 100 @ 100.00
    book.addOrder(1, Side::Buy, 1000000, 100, OrderType::Limit);
    
    const Order* order = book.getOrder(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remainingQty, 100);
    EXPECT_EQ(book.getBidLevelsCount(), 1);
}

TEST_F(OrderBookTest, ExecuteMatch_FullFill) {
    // Sell 100 @ 100.00
    book.addOrder(1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // Buy 100 @ 100.00
    book.addOrder(2, Side::Buy, 1000000, 100, OrderType::Limit);
    
    // Both should be filled and removed
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getOrder(2), nullptr);
    EXPECT_EQ(book.getAskLevelsCount(), 0);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}

TEST_F(OrderBookTest, ExecuteMatch_PartialFill) {
    // Sell 100 @ 100.00
    book.addOrder(1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // Buy 50 @ 100.00
    book.addOrder(2, Side::Buy, 1000000, 50, OrderType::Limit);
    
    // Order 1 should remain with 50
    const Order* order1 = book.getOrder(1);
    ASSERT_NE(order1, nullptr);
    EXPECT_EQ(order1->remainingQty, 50);
    
    // Order 2 should be gone
    EXPECT_EQ(book.getOrder(2), nullptr);
}

TEST_F(OrderBookTest, CancelOrder) {
    book.addOrder(1, Side::Buy, 1000000, 100, OrderType::Limit);
    book.cancelOrder(1);
    
    EXPECT_EQ(book.getOrder(1), nullptr);
    EXPECT_EQ(book.getBidLevelsCount(), 0);
}
