#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t { Buy, Sell };

// SR-F-01 (Limit) / SR-F-03 (Stop-Limit)
enum class OrderType : std::uint8_t { Limit, StopLimit };

// SR-I-03 execution status values.
//   RESTING          - accepted; sits in the book (Limit) or in the stop queue
//                      (Stop-Limit that has not been triggered yet).
//   PARTIALLY_FILLED - some, but not all, of the quantity has executed.
//   FILLED           - the whole quantity has executed.
//   CANCELLED        - removed from the engine by a cancel request.
//   REJECTED         - refused by the engine (4xx responses only; never part
//                      of a 200/201 response); see ExecutionReport::reason.
//
// A successful (200/201) response therefore only ever carries one of the four
// SR-I-03 statuses. Stop-Limit activation is reported separately through
// ExecutionReport::triggered, so an activated stop that merely rests in the
// book reports status RESTING with triggered == true.
enum class OrderStatus : std::uint8_t {
    RESTING,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    InvalidPrice,
    InvalidStopPrice,
    DuplicateOrderID,
    UnknownOrder
};

[[nodiscard]] constexpr std::string_view toString(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::RESTING:          return "RESTING";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view toString(Side side) noexcept {
    return side == Side::Buy ? "BUY" : "SELL";
}

[[nodiscard]] constexpr std::string_view toString(OrderType type) noexcept {
    return type == OrderType::Limit ? "LIMIT" : "STOP_LIMIT";
}

[[nodiscard]] constexpr std::string_view toString(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:             return "NONE";
        case RejectReason::InvalidQuantity:  return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice:     return "INVALID_PRICE";
        case RejectReason::InvalidStopPrice: return "INVALID_STOP_PRICE";
        case RejectReason::DuplicateOrderID: return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrder:     return "UNKNOWN_ORDER";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------

struct Order {
    std::uint64_t orderID{};
    double        orderPrice{};      // limit price
    double        stopPrice{0.0};    // trigger price; only meaningful for StopLimit
    std::uint64_t sequence{0};       // engine-assigned arrival sequence (strict FIFO key)
    std::uint32_t orderQuantity{};   // *remaining* quantity
    std::uint32_t originalQuantity{};
    Side          orderSide{Side::Buy};
    OrderType     orderType{OrderType::Limit};

    Order() = default;

    // Backwards compatible with the original 4-argument constructor.
    Order(std::uint64_t id, Side side, double price, std::uint32_t quantity,
          OrderType type = OrderType::Limit, double stop = 0.0) noexcept
        : orderID(id),
          orderPrice(price),
          stopPrice(stop),
          orderQuantity(quantity),
          originalQuantity(quantity),
          orderSide(side),
          orderType(type) {}

    [[nodiscard]] static Order limit(std::uint64_t id, Side side, double price,
                                     std::uint32_t quantity) noexcept {
        return Order(id, side, price, quantity, OrderType::Limit, 0.0);
    }

    [[nodiscard]] static Order stopLimit(std::uint64_t id, Side side, double stop,
                                         double price, std::uint32_t quantity) noexcept {
        return Order(id, side, price, quantity, OrderType::StopLimit, stop);
    }
};

// ---------------------------------------------------------------------------
// Execution reports (SR-I-03)
// ---------------------------------------------------------------------------

struct Trade {
    std::uint64_t buyOrderID{};
    std::uint64_t sellOrderID{};
    double        tradedPrice{};      // always the resting order's price
    std::uint32_t tradedQuantity{};
};

struct ExecutionReport {
    std::uint64_t orderID{};
    OrderStatus   status{OrderStatus::RESTING};
    std::uint32_t originalQuantity{};
    std::uint32_t executedQuantity{};
    std::uint32_t remainingQuantity{};
    double        price{};            // the order's limit price
    RejectReason  reason{RejectReason::None};
    bool          triggered{false};  // a Stop-Limit order that has been activated (SR-F-04)

    // Trades in which *this* order was the aggressor (empty => no heap use).
    std::vector<Trade> trades;

    // Stop-Limit orders that were activated as a consequence of this request
    // (including cascades), in activation order. Each entry is the activated
    // order's own report.
    std::vector<ExecutionReport> triggeredOrders;
};
