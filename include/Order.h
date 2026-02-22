#pragma once

#include "Types.h"
#include <atomic>

namespace OrderMatcher {

struct Order {
    OrderId id;
    ParticipantId participantId;
    Price price;
    Quantity initialQty;
    Quantity remainingQty;
    Side side;
    OrderType type;
    uint64_t timestamp;

    // Phase 6: Professional Features
    // Iceberg
    Quantity visibleQty = 0;
    Quantity displayQty = 0; // The amount to refresh

    // Stop
    Price stopPrice = 0;
    bool isStopTriggered = false;

    // Intrusive List Pointers
    Order* next = nullptr;
    Order* prev = nullptr;

    // Cache line alignment to prevent false sharing and optimize access
    alignas(64) char padding[0]; 
};

} // namespace OrderMatcher
