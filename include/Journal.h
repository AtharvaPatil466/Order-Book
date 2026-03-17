#pragma once

#include "Types.h"
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace OrderMatcher {

// CRC32 for journal entry integrity
inline uint32_t computeCRC32(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

// Binary journal entry for persistence/crash recovery
struct JournalEntry {
    enum class Type : uint8_t {
        AddOrder = 1,
        CancelOrder = 2,
        ModifyOrder = 3,
        CancelReplace = 4,
        Snapshot = 5    // Full order state for checkpoint
    };

    Type entryType;
    uint64_t sequenceNumber;
    uint64_t timestamp;

    // Order fields
    OrderId orderId;
    ParticipantId participantId;
    SymbolId symbolId;
    Side side;
    Price price;
    Quantity quantity;
    OrderType orderType;
    TimeInForce timeInForce;
    uint64_t expiryTime;
    Price stopPrice;
    Price stopLimitPrice;
    Quantity displayQty;
    PegType pegType;
    Price pegOffset;
    Price trailAmount;
    Quantity minQty;
    bool hidden;

    // For modify/cancel-replace
    Price newPrice;
    Quantity newQty;

    // CRC32 checksum (must be last field — computed over all preceding bytes)
    uint32_t checksum;
};

class Journal {
public:
    explicit Journal(const std::string& filePath)
        : filePath_(filePath), sequence_(0) {
        file_ = std::fopen(filePath.c_str(), "ab+");
    }

    ~Journal() {
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
        }
    }

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    void logAddOrder(OrderId id, ParticipantId pid, SymbolId sym, Side side, Price price,
                     Quantity qty, OrderType type, TimeInForce tif = TimeInForce::GTC,
                     uint64_t expiry = 0, Price stopPrice = 0, Price stopLimitPrice = 0,
                     Quantity displayQty = 0, PegType pegType = PegType::None,
                     Price pegOffset = 0, Price trailAmount = 0, Quantity minQty = 0,
                     bool hidden = false) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::AddOrder;
        entry.sequenceNumber = ++sequence_;
        entry.timestamp = now();
        entry.orderId = id;
        entry.participantId = pid;
        entry.symbolId = sym;
        entry.side = side;
        entry.price = price;
        entry.quantity = qty;
        entry.orderType = type;
        entry.timeInForce = tif;
        entry.expiryTime = expiry;
        entry.stopPrice = stopPrice;
        entry.stopLimitPrice = stopLimitPrice;
        entry.displayQty = displayQty;
        entry.pegType = pegType;
        entry.pegOffset = pegOffset;
        entry.trailAmount = trailAmount;
        entry.minQty = minQty;
        entry.hidden = hidden;
        writeEntry(entry);
    }

    void logCancelOrder(OrderId id) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelOrder;
        entry.sequenceNumber = ++sequence_;
        entry.timestamp = now();
        entry.orderId = id;
        writeEntry(entry);
    }

    void logModifyOrder(OrderId id, Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::ModifyOrder;
        entry.sequenceNumber = ++sequence_;
        entry.timestamp = now();
        entry.orderId = id;
        entry.newQty = newQty;
        writeEntry(entry);
    }

    void logCancelReplace(OrderId id, Price newPrice, Quantity newQty) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::CancelReplace;
        entry.sequenceNumber = ++sequence_;
        entry.timestamp = now();
        entry.orderId = id;
        entry.newPrice = newPrice;
        entry.newQty = newQty;
        writeEntry(entry);
    }

    // Write a snapshot entry (checkpoint of a single order's current state)
    void logSnapshot(OrderId id, ParticipantId pid, SymbolId sym, Side side, Price price,
                     Quantity remainingQty, OrderType type, TimeInForce tif = TimeInForce::GTC,
                     uint64_t expiry = 0, Price stopPrice = 0, Price stopLimitPrice = 0,
                     Quantity displayQty = 0, PegType pegType = PegType::None,
                     Price pegOffset = 0, Price trailAmount = 0, Quantity minQty = 0,
                     bool hidden = false) {
        JournalEntry entry{};
        entry.entryType = JournalEntry::Type::Snapshot;
        entry.sequenceNumber = ++sequence_;
        entry.timestamp = now();
        entry.orderId = id;
        entry.participantId = pid;
        entry.symbolId = sym;
        entry.side = side;
        entry.price = price;
        entry.quantity = remainingQty;
        entry.orderType = type;
        entry.timeInForce = tif;
        entry.expiryTime = expiry;
        entry.stopPrice = stopPrice;
        entry.stopLimitPrice = stopLimitPrice;
        entry.displayQty = displayQty;
        entry.pegType = pegType;
        entry.pegOffset = pegOffset;
        entry.trailAmount = trailAmount;
        entry.minQty = minQty;
        entry.hidden = hidden;
        writeEntry(entry);
    }

    // Read all entries, validating CRC. Returns only valid entries.
    std::vector<JournalEntry> readAll(bool validateCRC = true) {
        std::vector<JournalEntry> entries;
        if (!file_) return entries;

        std::fseek(file_, 0, SEEK_SET);
        JournalEntry entry;
        while (std::fread(&entry, sizeof(JournalEntry), 1, file_) == 1) {
            if (validateCRC) {
                size_t dataSize = offsetof(JournalEntry, checksum);
                uint32_t expected = computeCRC32(&entry, dataSize);
                if (entry.checksum != expected) {
                    // Corrupted entry — stop reading (truncated write)
                    break;
                }
            }
            entries.push_back(entry);
        }
        std::fseek(file_, 0, SEEK_END);
        return entries;
    }

    void flush() {
        if (file_) std::fflush(file_);
    }

    // Truncate after checkpoint
    void truncate() {
        if (file_) {
            std::fclose(file_);
            file_ = std::fopen(filePath_.c_str(), "wb+");
            sequence_ = 0;
        }
    }

    uint64_t getSequence() const { return sequence_; }

private:
    void writeEntry(JournalEntry& entry) {
        if (!file_) return;
        // Compute CRC over all fields except checksum
        size_t dataSize = offsetof(JournalEntry, checksum);
        entry.checksum = computeCRC32(&entry, dataSize);
        std::fwrite(&entry, sizeof(JournalEntry), 1, file_);
    }

    static uint64_t now() {
        return static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }

    FILE* file_ = nullptr;
    std::string filePath_;
    uint64_t sequence_;
};

} // namespace OrderMatcher
