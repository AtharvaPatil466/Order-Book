#include <iostream>
#include <cassert>
#include <random>
#include <vector>
#include <algorithm>
#include <deque>
#include "OrderBook.h"
#include "Types.h"

using namespace OrderMatcher;

void testBasicMatching() {
    std::cout << "Running testBasicMatching..." << std::endl;
    OrderBook book;
    
    // Participant 1 Sell 100 @ 100.00
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // Participant 2 Buy 100 @ 100.00
    book.addOrder(2, 2, Side::Buy, 1000000, 100, OrderType::Limit);
    
    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getAskLevelsCount() == 0);
    assert(book.getBidLevelsCount() == 0);
    std::cout << "testBasicMatching PASSED" << std::endl;
}

void testSMP() {
    std::cout << "Running testSMP..." << std::endl;
    OrderBook book;
    
    // Participant 1 Sell 100 @ 100.00
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // Participant 1 Buy 100 @ 100.00 (Should trigger SMP and cancel incoming)
    book.addOrder(2, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    
    assert(book.getOrder(1) != nullptr); // Resting order should remain
    assert(book.getOrder(2) == nullptr); // Incoming order should be cancelled
    assert(book.getAskLevelsCount() == 1);
    std::cout << "testSMP PASSED" << std::endl;
}

void testMarketOrder() {
    std::cout << "Running testMarketOrder..." << std::endl;
    OrderBook book;
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit); // 100 @ 100.00
    book.addOrder(2, 2, Side::Sell, 1010000, 100, OrderType::Limit); // 100 @ 101.00
    
    // Market Buy 150
    book.addOrder(3, 3, Side::Buy, 0, 150, OrderType::Market);
    
    assert(book.getOrder(1) == nullptr);
    const Order* order2 = book.getOrder(2);
    assert(order2 != nullptr && order2->remainingQty == 50);
    assert(book.getAskLevelsCount() == 1);
    std::cout << "testMarketOrder PASSED" << std::endl;
}

void testIOC() {
    std::cout << "Running testIOC..." << std::endl;
    OrderBook book;
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // IOC Buy 150 @ 100.00 (Should match 100 and cancel 50)
    book.addOrder(2, 2, Side::Buy, 1000000, 150, OrderType::IOC);
    
    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(2) == nullptr);
    assert(book.getBidLevelsCount() == 0);
    std::cout << "testIOC PASSED" << std::endl;
}

void testFOK() {
    std::cout << "Running testFOK..." << std::endl;
    OrderBook book;
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // FOK Buy 150 @ 100.00 (Should KILL because not enough liquidity)
    book.addOrder(2, 2, Side::Buy, 1000000, 150, OrderType::FOK);
    
    assert(book.getOrder(1) != nullptr); // 100 @ 100.00 remains
    assert(book.getOrder(2) == nullptr);
    
    // FOK Buy 100 @ 100.00 (Should FILL)
    book.addOrder(3, 3, Side::Buy, 1000000, 100, OrderType::FOK);
    assert(book.getOrder(1) == nullptr);
    assert(book.getOrder(3) == nullptr);
    std::cout << "testFOK PASSED" << std::endl;
}

void testStopOrder() {
    std::cout << "Running testStopOrder..." << std::endl;
    OrderBook book;
    
    // Stop Buy @ 105.00
    book.addOrder(1, 1, Side::Buy, 1050000, 100, OrderType::Stop, 1050000);
    
    // Trade occurs at 104.00 (No trigger)
    book.addOrder(2, 2, Side::Sell, 1040000, 50, OrderType::Limit);
    book.addOrder(3, 3, Side::Buy, 1040000, 50, OrderType::Limit);
    assert(book.getOrder(1) != nullptr);
    
    // Trade occurs at 105.00 (TRIGGERS!)
    book.addOrder(4, 4, Side::Sell, 1050000, 50, OrderType::Limit);
    book.addOrder(5, 5, Side::Buy, 1050000, 50, OrderType::Limit);
    
    // Order 1 should now be in the book as a Limit Buy @ 105.00
    assert(book.getBidLevelsCount() == 1);
    const Order* order1 = book.getOrder(1);
    assert(order1 != nullptr && order1->type == OrderType::Limit);
    std::cout << "testStopOrder PASSED" << std::endl;
}

void testIcebergOrder() {
    std::cout << "Running testIcebergOrder..." << std::endl;
    OrderBook book;
    
    // Iceberg Sell 100, Visible 30 @ 100.00
    book.addOrder(1, 1, Side::Sell, 1000000, 100, OrderType::Iceberg, 0, 30);
    
    // Match 20 (Visible remains 10)
    book.addOrder(2, 2, Side::Buy, 1000000, 20, OrderType::Limit);
    const Order* order1 = book.getOrder(1);
    assert(order1->visibleQty == 10);
    assert(order1->remainingQty == 80);
    
    // Match 15 (Visible refills to 30, loses priority?)
    // In this test, it's the only one, so priority doesn't matter much yet
    book.addOrder(3, 3, Side::Buy, 1000000, 15, OrderType::Limit);
    assert(order1->visibleQty == 25); // 10-15 = refills from reserve. Wait, 10 - 15? 
    // Logic was: fillQty = min(incoming, visible). 
    // incoming 15, visible 10. fillQty = 10.
    // remainingQty 80-10=70. visible 10-10=0.
    // refill: visible = min(70, 30) = 30.
    // incoming remaining = 15-10 = 5.
    // Next iteration of match: bookOrder is still order1 (it moved to back but only child).
    // visible 30. fillQty = min(5, 30) = 5.
    // remainingQty 70-5=65. visible 30-5=25.
    assert(order1->visibleQty == 25);
    assert(order1->remainingQty == 65);
    std::cout << "testIcebergOrder PASSED" << std::endl;
}

void testAnalytics() {
    std::cout << "Running testAnalytics..." << std::endl;
    OrderBook book;
    
    // Trade @ 100
    book.addOrder(1, 10, Side::Sell, 1000000, 100, OrderType::Limit);
    book.addOrder(2, 20, Side::Buy, 1000000, 100, OrderType::Limit);
    assert(book.getVWAP() == 1000000);
    
    // Trade @ 102
    book.addOrder(3, 10, Side::Sell, 1020000, 100, OrderType::Limit);
    book.addOrder(4, 30, Side::Buy, 1020000, 100, OrderType::Limit);
    assert(book.getVWAP() == 1010000);
    
    // OTR Check
    // Participant 10: 2 Adds (1, 3), 2 Trades. Ratio 1.0
    // Participant 30: 1 Add (4), 1 Trade. Ratio 1.0
    assert(book.getOTR(10) == 1.0);
    assert(book.getOTR(30) == 1.0);
    
    std::cout << "testAnalytics PASSED" << std::endl;
}

void testCircuitBreaker() {
    std::cout << "Running testCircuitBreaker..." << std::endl;
    OrderBook book;
    
    // Anchor price @ 100
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    
    // Order @ 106 (6% deviation) -> Should HALT
    book.addOrder(2, 2, Side::Buy, 1060000, 100, OrderType::Limit);
    
    // Subsequent order should be rejected
    book.addOrder(3, 3, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(book.getOrder(3) == nullptr);
    std::cout << "testCircuitBreaker PASSED" << std::endl;
}

void testStress() {
    std::cout << "Running testStress (10,000 random ops)..." << std::endl;
    OrderBook book;
    std::mt19937 rng(1337);
    std::uniform_int_distribution<Price> priceDist(990000, 1010000);
    std::uniform_int_distribution<int> opDist(1, 100);
    std::vector<OrderId> orders;

    for (int i = 0; i < 10000; ++i) {
        int op = opDist(rng);
        if (op <= 70) { // Add
            OrderId id = i + 1;
            orders.push_back(id);
            book.addOrder(id, 1, (i % 2 == 0 ? Side::Buy : Side::Sell), priceDist(rng), 10, OrderType::Limit);
        } else if (!orders.empty()) { // Cancel
            size_t idx = rng() % orders.size();
            book.cancelOrder(orders[idx]);
            orders[idx] = orders.back();
            orders.pop_back();
        }
    }
    std::cout << "testStress PASSED" << std::endl;
}

void testPartialFillPriority() {
    std::cout << "Running testPartialFillPriority..." << std::endl;
    OrderBook book;
    
    // 1. Add Buy 100 @ 100 (Order 1)
    book.addOrder(1, 10, Side::Buy, 1000000, 100, OrderType::Limit);
    // 2. Add Buy 100 @ 100 (Order 2)
    book.addOrder(2, 20, Side::Buy, 1000000, 100, OrderType::Limit);
    
    // 3. Sell 50 @ 100 -> Should match Order 1 partially
    book.addOrder(3, 30, Side::Sell, 1000000, 50, OrderType::Limit);
    
    // 4. Verify Order 1 still has 50 left and is still at the top
    assert(book.getTradeHistory().back().buyOrderId == 1);
    assert(book.getTradeHistory().back().quantity == 50);
    
    // 5. Sell 100 @ 100 -> Should match remaining 50 of Order 1, then 50 of Order 2
    book.addOrder(4, 30, Side::Sell, 1000000, 100, OrderType::Limit);
    auto history = book.getTradeHistory();
    // history[-1] is Sell(4) vs Buy(2) for 50
    // history[-2] is Sell(4) vs Buy(1) for 50
    assert(history[history.size()-2].buyOrderId == 1);
    assert(history[history.size()-1].buyOrderId == 2);
    
    std::cout << "testPartialFillPriority PASSED" << std::endl;
}

void testFuzz(int numOps) {
    std::cout << "Running testFuzz (" << numOps << " ops)..." << std::endl;
    OrderBook book;
    std::mt19937 rng(42);
    std::uniform_int_distribution<Price> priceDist(990, 1010);
    std::uniform_int_distribution<int> opDist(1, 100);
    std::deque<OrderId> activeOrders;
    OrderId nextId = 1;

    for (int i = 0; i < numOps; ++i) {
        int op = opDist(rng);
        if (op <= 50) { // Add
            OrderId id = nextId++;
            activeOrders.push_back(id);
            book.addOrder(id, (i%10)+1, (i%2==0 ? Side::Buy : Side::Sell), priceDist(rng)*1000, 100, OrderType::Limit);
        } else if (op <= 90 && !activeOrders.empty()) { // Cancel
            size_t idx = rng() % activeOrders.size();
            book.cancelOrder(activeOrders[idx]);
            activeOrders.erase(activeOrders.begin() + idx);
        } else { // Market
            book.addOrder(nextId++, 99, (i%2==0 ? Side::Buy : Side::Sell), 0, 50, OrderType::Market);
        }
        
        // Invariant check: Best Bid < Best Ask
        Price bb = book.getBestBid();
        Price ba = book.getBestAsk();
        if (bb != 0 && ba != std::numeric_limits<Price>::max()) {
            assert(bb < ba);
        }
    }
    std::cout << "testFuzz PASSED" << std::endl;
}

void testSMPPurity() {
    std::cout << "Running testSMPPurity..." << std::endl;
    OrderBook book;
    // Participant 1 adds a Buy order
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    // Participant 1 adds a Sell order at the same price -> Should NOT match
    book.addOrder(2, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    assert(book.getTradeCount() == 0);
    assert(book.getBidLevelsCount() == 1);
    assert(book.getAskLevelsCount() == 0); // Corrected: Taker is cancelled on SMP
    
    // Participant 2 adds a Sell order at the same price -> SHOULD match
    book.addOrder(3, 2, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(book.getTradeCount() == 1);
    
    std::cout << "testSMPPurity PASSED" << std::endl;
}

void testCircuitBreakerBoundaries() {
    std::cout << "Running testCircuitBreakerBoundaries..." << std::endl;
    OrderBook book;
    // Reference price set at 1,000,000 via first order
    book.addOrder(1, 1, Side::Buy, 1000000, 100, OrderType::Limit);
    
    // 4.9% movement -> Should be OK
    book.addOrder(2, 2, Side::Sell, 1049000, 100, OrderType::Limit);
    assert(book.getTradeCount() == 0); // No match but order accepted
    
    // 5.1% movement -> Should trigger HALT
    book.addOrder(3, 3, Side::Buy, 1051000, 100, OrderType::Limit);
    
    // Try to match in halted state
    book.addOrder(4, 4, Side::Sell, 1000000, 100, OrderType::Limit);
    assert(book.getTradeCount() == 0); // No match because it's halted or rejected
    
    std::cout << "testCircuitBreakerBoundaries PASSED" << std::endl;
}

int main() {
    try {
        testBasicMatching();
        testSMP();
        testSMPPurity();
        testMarketOrder();
        testIOC();
        testFOK();
        testStopOrder();
        testIcebergOrder();
        testAnalytics();
        testCircuitBreaker();
        testCircuitBreakerBoundaries();
        testStress();
        testPartialFillPriority();
        testFuzz(100000); 
        std::cout << "\nALL TESTS PASSED!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
