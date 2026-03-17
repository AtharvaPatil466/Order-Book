#pragma once

#include "Types.h"
#include "OrderBook.h"
#include <string>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <cstdlib>

namespace OrderMatcher {

// FIX Tag Constants
namespace FIXTag {
    constexpr int BeginString   = 8;
    constexpr int BodyLength    = 9;
    constexpr int MsgType       = 35;
    constexpr int SenderCompID  = 49;
    constexpr int TargetCompID  = 56;
    constexpr int Checksum      = 10;
    constexpr int ClOrdID       = 11;
    constexpr int OrigClOrdID   = 41;
    constexpr int OrderID       = 37;
    constexpr int ExecID        = 17;
    constexpr int ExecType      = 150;
    constexpr int OrdStatus     = 39;
    constexpr int Account       = 1;
    constexpr int Symbol        = 55;
    constexpr int Side          = 54;
    constexpr int Price         = 44;
    constexpr int OrderQty      = 38;
    constexpr int OrdType       = 40;
    constexpr int TimeInForce   = 59;
    constexpr int StopPx        = 99;
    constexpr int MinQty        = 110;
    constexpr int MaxFloor      = 111;
    constexpr int ExpireDate    = 432;
    constexpr int ExecInst      = 18;
    constexpr int CumQty        = 14;
    constexpr int LeavesQty     = 151;
    constexpr int LastPx        = 31;
    constexpr int LastQty       = 32;
}

// FIX MsgType values
namespace FIXMsgType {
    const std::string NewOrderSingle        = "D";
    const std::string OrderCancelRequest    = "F";
    const std::string OrderCancelReplace    = "G";
    const std::string ExecutionReport       = "8";
}

// ─── FIXMessage: parse and build FIX tag=value messages ─────────────────────

class FIXMessage {
public:
    FIXMessage() = default;

    // Parse a raw FIX string. Supports SOH (0x01) or pipe '|' as delimiter.
    static FIXMessage parse(const std::string& raw) {
        FIXMessage msg;
        char delim = '\x01';
        if (raw.find('|') != std::string::npos && raw.find('\x01') == std::string::npos)
            delim = '|';

        size_t pos = 0;
        while (pos < raw.size()) {
            size_t eq = raw.find('=', pos);
            if (eq == std::string::npos) break;
            size_t end = raw.find(delim, eq);
            if (end == std::string::npos) end = raw.size();

            int tag = std::atoi(raw.substr(pos, eq - pos).c_str());
            std::string value = raw.substr(eq + 1, end - eq - 1);
            msg.fields_[tag] = value;

            pos = end + 1;
        }
        return msg;
    }

    // Build a FIX string with SOH delimiter, including BeginString, BodyLength, Checksum
    std::string build(const std::string& senderCompID = "", const std::string& targetCompID = "") const {
        // Build body (tags 35 onward, excluding 8, 9, 10)
        std::string body;
        // MsgType must be first in body
        if (hasField(FIXTag::MsgType))
            body += "35=" + getField(FIXTag::MsgType) + '\x01';

        for (const auto& [tag, val] : fields_) {
            if (tag == FIXTag::BeginString || tag == FIXTag::BodyLength
                || tag == FIXTag::Checksum || tag == FIXTag::MsgType)
                continue;
            body += std::to_string(tag) + "=" + val + '\x01';
        }

        if (!senderCompID.empty())
            body += "49=" + senderCompID + '\x01';
        if (!targetCompID.empty())
            body += "56=" + targetCompID + '\x01';

        // Header: 8=FIX.4.4 | 9=<bodylen>
        std::string header = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + '\x01';

        // Checksum: sum of all bytes mod 256
        std::string preChecksum = header + body;
        uint32_t sum = 0;
        for (char c : preChecksum) sum += static_cast<uint8_t>(c);
        std::ostringstream cksum;
        cksum << std::setw(3) << std::setfill('0') << (sum % 256);

        return preChecksum + "10=" + cksum.str() + '\x01';
    }

    void setField(int tag, const std::string& value) { fields_[tag] = value; }
    void setField(int tag, int64_t value) { fields_[tag] = std::to_string(value); }
    void setField(int tag, double value) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4) << value;
        fields_[tag] = ss.str();
    }

    std::string getField(int tag) const {
        auto it = fields_.find(tag);
        return (it != fields_.end()) ? it->second : "";
    }

    bool hasField(int tag) const { return fields_.count(tag) > 0; }

    int64_t getInt(int tag) const { return std::strtoll(getField(tag).c_str(), nullptr, 10); }
    double getDouble(int tag) const { return std::strtod(getField(tag).c_str(), nullptr); }

private:
    std::unordered_map<int, std::string> fields_;
};

// ─── FIXAdapter: convert between FIX messages and internal types ────────────

class FIXAdapter {
public:
    // Parse NewOrderSingle (35=D) into addOrder parameters
    struct NewOrderParams {
        OrderId orderId;
        ParticipantId participantId;
        SymbolId symbolId;
        OrderMatcher::Side side;
        Price price;
        Quantity qty;
        OrderType type;
        TimeInForce tif;
        Price stopPrice;
        Quantity displayQty;
        Quantity minQty;
        bool hidden;
        uint64_t expiryTime;
    };

    static NewOrderParams parseNewOrder(const FIXMessage& msg) {
        NewOrderParams p{};
        p.orderId = static_cast<OrderId>(msg.getInt(FIXTag::ClOrdID));
        p.participantId = static_cast<ParticipantId>(msg.getInt(FIXTag::Account));
        p.symbolId = static_cast<SymbolId>(msg.getInt(FIXTag::Symbol));
        p.price = static_cast<Price>(msg.getDouble(FIXTag::Price) * PRICE_PRECISION + 0.5);
        p.qty = static_cast<Quantity>(msg.getInt(FIXTag::OrderQty));
        p.stopPrice = static_cast<Price>(msg.getDouble(FIXTag::StopPx) * PRICE_PRECISION + 0.5);
        p.displayQty = static_cast<Quantity>(msg.getInt(FIXTag::MaxFloor));
        p.minQty = static_cast<Quantity>(msg.getInt(FIXTag::MinQty));
        p.expiryTime = static_cast<uint64_t>(msg.getInt(FIXTag::ExpireDate));

        // Side: 1=Buy, 2=Sell
        int s = static_cast<int>(msg.getInt(FIXTag::Side));
        p.side = (s == 2) ? OrderMatcher::Side::Sell : OrderMatcher::Side::Buy;

        // OrdType: 1=Market, 2=Limit, 3=Stop, 4=StopLimit, P=Pegged
        std::string ot = msg.getField(FIXTag::OrdType);
        if (ot == "1") p.type = OrderType::Market;
        else if (ot == "3") p.type = OrderType::Stop;
        else if (ot == "4") p.type = OrderType::StopLimit;
        else if (ot == "P") p.type = OrderType::Pegged;
        else p.type = OrderType::Limit; // default

        // TimeInForce: 0=Day, 1=GTC, 3=IOC, 4=FOK, 6=GTD
        int tf = static_cast<int>(msg.getInt(FIXTag::TimeInForce));
        if (tf == 3) { p.type = OrderType::IOC; p.tif = OrderMatcher::TimeInForce::GTC; }
        else if (tf == 4) { p.type = OrderType::FOK; p.tif = OrderMatcher::TimeInForce::GTC; }
        else if (tf == 0) p.tif = OrderMatcher::TimeInForce::DAY;
        else if (tf == 6) p.tif = OrderMatcher::TimeInForce::GTD;
        else p.tif = OrderMatcher::TimeInForce::GTC;

        // ExecInst: H=Hidden
        p.hidden = (msg.getField(FIXTag::ExecInst) == "H");

        // Iceberg: if MaxFloor is set and type is Limit, treat as Iceberg
        if (p.displayQty > 0 && p.type == OrderType::Limit)
            p.type = OrderType::Iceberg;

        return p;
    }

    // Parse OrderCancelRequest (35=F)
    struct CancelParams {
        OrderId orderId;
        SymbolId symbolId;
    };

    static CancelParams parseCancelRequest(const FIXMessage& msg) {
        CancelParams p{};
        p.orderId = static_cast<OrderId>(msg.getInt(FIXTag::ClOrdID));
        p.symbolId = static_cast<SymbolId>(msg.getInt(FIXTag::Symbol));
        return p;
    }

    // Parse OrderCancelReplaceRequest (35=G)
    struct CancelReplaceParams {
        OrderId origOrderId;
        SymbolId symbolId;
        Price newPrice;
        Quantity newQty;
    };

    static CancelReplaceParams parseCancelReplace(const FIXMessage& msg) {
        CancelReplaceParams p{};
        p.origOrderId = static_cast<OrderId>(msg.getInt(FIXTag::OrigClOrdID));
        p.symbolId = static_cast<SymbolId>(msg.getInt(FIXTag::Symbol));
        p.newPrice = static_cast<Price>(msg.getDouble(FIXTag::Price) * PRICE_PRECISION + 0.5);
        p.newQty = static_cast<Quantity>(msg.getInt(FIXTag::OrderQty));
        return p;
    }

    // Build ExecutionReport (35=8) from a Trade
    static FIXMessage buildTradeReport(const Trade& trade) {
        FIXMessage msg;
        msg.setField(FIXTag::MsgType, FIXMsgType::ExecutionReport);
        msg.setField(FIXTag::OrderID, static_cast<int64_t>(trade.buyOrderId));
        msg.setField(FIXTag::ExecID, static_cast<int64_t>(trade.tradeId));
        msg.setField(FIXTag::ExecType, "F"); // Fill
        msg.setField(FIXTag::OrdStatus, "2"); // Filled (simplified)
        msg.setField(FIXTag::Symbol, static_cast<int64_t>(trade.symbolId));
        msg.setField(FIXTag::LastPx, toDouble(trade.price));
        msg.setField(FIXTag::LastQty, static_cast<int64_t>(trade.quantity));
        return msg;
    }

    // Build ExecutionReport (35=8) from an OrderUpdate
    static FIXMessage buildOrderReport(const OrderUpdate& update) {
        FIXMessage msg;
        msg.setField(FIXTag::MsgType, FIXMsgType::ExecutionReport);
        msg.setField(FIXTag::OrderID, static_cast<int64_t>(update.orderId));
        msg.setField(FIXTag::CumQty, static_cast<int64_t>(update.filledQty));
        msg.setField(FIXTag::LeavesQty, static_cast<int64_t>(update.remainingQty));

        // Map OrderStatus to FIX OrdStatus
        switch (update.status) {
            case OrderStatus::New:
            case OrderStatus::Accepted:
                msg.setField(FIXTag::OrdStatus, "0");
                msg.setField(FIXTag::ExecType, "0");
                break;
            case OrderStatus::PartiallyFilled:
                msg.setField(FIXTag::OrdStatus, "1");
                msg.setField(FIXTag::ExecType, "F");
                break;
            case OrderStatus::Filled:
                msg.setField(FIXTag::OrdStatus, "2");
                msg.setField(FIXTag::ExecType, "F");
                break;
            case OrderStatus::Cancelled:
                msg.setField(FIXTag::OrdStatus, "4");
                msg.setField(FIXTag::ExecType, "4");
                break;
            case OrderStatus::Rejected:
                msg.setField(FIXTag::OrdStatus, "8");
                msg.setField(FIXTag::ExecType, "8");
                break;
        }

        if (update.lastFillPrice > 0)
            msg.setField(FIXTag::LastPx, toDouble(update.lastFillPrice));

        return msg;
    }
};

} // namespace OrderMatcher
