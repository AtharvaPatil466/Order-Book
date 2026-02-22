#include "MatchingEngine.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace OrderMatcher;

int main() {
    std::cout << "Initializing Low-Latency Order Matching Engine..." << std::endl;

    MatchingEngine engine;
    engine.start();

    // Warmup
    std::cout << "Warming up..." << std::endl;
    for (int i = 0; i < 1000; ++i) {
        engine.processOrder(i, 1, Side::Buy, 100000 + (i % 100), 10, OrderType::Limit);
        engine.cancelOrder(i);
    }

    std::cout << "Running basic scenario..." << std::endl;

    // 1. Add functionality test
    // Participant 1 Sell 100 @ 100.00
    engine.processOrder(1001, 1, Side::Sell, 1000000, 100, OrderType::Limit);
    
    // Participant 2 Buy 50 @ 100.00 (Match 50)
    engine.processOrder(1002, 2, Side::Buy, 1000000, 50, OrderType::Limit);
    
    // Participant 3 Buy 60 @ 100.00 (Match 50, Rest 10)
    engine.processOrder(1003, 3, Side::Buy, 1000000, 60, OrderType::Limit);

    std::cout << "Scenario completed." << std::endl;
    
    engine.stop();
    return 0;
}
