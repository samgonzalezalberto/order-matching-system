#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "order.hpp"

// SR-D-01: ordered balanced trees of FIFO queues. Bids are best-first (highest
// price), asks are best-first (lowest price).
using BidLevels = std::map<double, std::list<Order>, std::greater<double>>;
using AskLevels = std::map<double, std::list<Order>>;

// SR-D-02: direct-lookup record for every active order id.
//
// std::map / std::list iterators are stable under insert/erase of *other*
// elements, so storing the tier iterator alongside the list iterator lets
// cancellation erase an emptied price tier without a second O(log n) lookup.
struct OrderLocation {
    double price{};
    Side   side{Side::Buy};
    bool   isStop{false};
    std::list<Order>::iterator listIterator{};                        // book orders
    // Alternative 0 = bid tier, 1 = ask tier. Accessed by *index*: the two map
    // iterator types may be identical (they are on libc++/libstdc++), which
    // would make by-type access ambiguous.
    using Tier = std::variant<BidLevels::iterator, AskLevels::iterator>;
    Tier tier{};                                                      // book orders
    std::size_t stopIndex{0};                                         // stop orders
};

// Point-in-time view of the book. quantity totals are O(n) to compute, so
// treat stats() as a diagnostic/test facility, not a hot-path call.
struct BookStats {
    std::optional<double> bestBid;
    std::optional<double> bestAsk;
    std::optional<double> lastTradedPrice;
    std::size_t           bidLevels{0};
    std::size_t           askLevels{0};
    std::size_t           restingOrders{0};   // limit orders in the book
    std::size_t           stopOrders{0};      // untriggered stop-limit orders
    std::uint64_t         bidQuantity{0};
    std::uint64_t         askQuantity{0};
};

// Single-threaded matching core. Use MatchingEngine (matching_engine.hpp) for
// the thread-safe façade.
class OrderBook {
public:
    explicit OrderBook(std::size_t expectedOrders = 1u << 16) {
        // Avoid rehashing / reallocation on the hot path.
        orderIndex.reserve(expectedOrders);
        buyStopOrders.reserve(256);
        sellStopOrders.reserve(256);
        triggerScratch.reserve(64);
    }

    // SR-F-01..07, SR-I-03. The returned report describes the submitted order;
    // any Stop-Limit orders it activated are listed in `triggeredOrders`.
    ExecutionReport addOrder(Order order) {
        if (const RejectReason reason = validate(order); reason != RejectReason::None) {
            ExecutionReport rejected;
            rejected.orderID = order.orderID;
            rejected.status = OrderStatus::REJECTED;
            rejected.reason = reason;
            rejected.originalQuantity = order.orderQuantity;
            rejected.price = order.orderPrice;
            return rejected;
        }

        order.sequence = nextSequence++;
        order.originalQuantity = order.orderQuantity;
        ExecutionReport report = makeReport(order);

        const bool isStop = order.orderType == OrderType::StopLimit;
        if (isStop && !stopConditionMet(order)) {
            // SR-F-04: hold in the secondary evaluation queue. Parking an
            // order cannot produce a trade, so there is nothing to cascade.
            parkStopOrder(order);
            report.status = OrderStatus::RESTING;
            return report;
        }

        PriceRange traded;
        executeLimit(order, report, traded);
        if (isStop) report.triggered = true;   // activated on arrival

        evaluateStopOrders(traded, report);
        return report;
    }

    // SR-F-08 / SR-F-09: O(1) average removal for limit *and* stop orders.
    ExecutionReport cancelOrder(std::uint64_t orderID) {
        ExecutionReport report;
        report.orderID = orderID;

        const auto found = orderIndex.find(orderID);
        if (found == orderIndex.end()) {
            report.status = OrderStatus::REJECTED;
            report.reason = RejectReason::UnknownOrder;
            return report;
        }

        const OrderLocation location = found->second;   // copy: the entry is erased below
        Order removed;
        if (location.isStop) {
            removed = removeStopAt(location.side == Side::Buy ? buyStopOrders : sellStopOrders,
                                   location.stopIndex);   // also erases the index entry
        } else {
            removed = location.side == Side::Buy ? eraseFromLevel<0>(bids, location)
                                                 : eraseFromLevel<1>(asks, location);
            orderIndex.erase(found);
        }

        report.status = OrderStatus::CANCELLED;
        report.originalQuantity = removed.originalQuantity;
        report.remainingQuantity = removed.orderQuantity;
        report.executedQuantity = removed.originalQuantity - removed.orderQuantity;
        report.price = removed.orderPrice;
        return report;
    }

    // ---- read-only accessors ------------------------------------------------

    [[nodiscard]] bool contains(std::uint64_t orderID) const { return orderIndex.contains(orderID); }
    [[nodiscard]] std::size_t activeOrderCount() const noexcept { return orderIndex.size(); }
    [[nodiscard]] std::size_t stopOrderCount() const noexcept {
        return buyStopOrders.size() + sellStopOrders.size();
    }
    [[nodiscard]] std::size_t bidLevelCount() const noexcept { return bids.size(); }
    [[nodiscard]] std::size_t askLevelCount() const noexcept { return asks.size(); }
    [[nodiscard]] std::optional<double> bestBid() const {
        return bids.empty() ? std::nullopt : std::optional<double>(bids.begin()->first);
    }
    [[nodiscard]] std::optional<double> bestAsk() const {
        return asks.empty() ? std::nullopt : std::optional<double>(asks.begin()->first);
    }
    [[nodiscard]] std::optional<double> lastPrice() const {
        return hasTraded ? std::optional<double>(lastTradedPrice) : std::nullopt;
    }

    [[nodiscard]] BookStats stats() const {
        BookStats s;
        s.bestBid = bestBid();
        s.bestAsk = bestAsk();
        s.lastTradedPrice = lastPrice();
        s.bidLevels = bids.size();
        s.askLevels = asks.size();
        s.stopOrders = stopOrderCount();
        s.restingOrders = orderIndex.size() - s.stopOrders;
        for (const auto& [price, queue] : bids)
            for (const Order& o : queue) s.bidQuantity += o.orderQuantity;
        for (const auto& [price, queue] : asks)
            for (const Order& o : queue) s.askQuantity += o.orderQuantity;
        return s;
    }

private:
    // Lowest/highest trade price seen while processing one batch of executions.
    struct PriceRange {
        double low{std::numeric_limits<double>::infinity()};
        double high{-std::numeric_limits<double>::infinity()};
        bool   valid{false};

        void add(double price) noexcept {
            low = std::min(low, price);
            high = std::max(high, price);
            valid = true;
        }
    };

    static constexpr double kInf = std::numeric_limits<double>::infinity();

    // ---- state ---------------------------------------------------------------
    BidLevels bids;
    AskLevels asks;
    std::unordered_map<std::uint64_t, OrderLocation> orderIndex;

    // Secondary stop-limit queues (SR-F-04).
    std::vector<Order> buyStopOrders;    // trigger when lastTradedPrice >= stopPrice
    std::vector<Order> sellStopOrders;   // trigger when lastTradedPrice <= stopPrice
    // Cached bounds that let a trade skip scanning the queues in O(1) when no
    // stop can possibly fire. They are always a valid bound (never tighter than
    // the true extreme) and are made exact whenever a queue is scanned.
    double minBuyStop{kInf};
    double maxSellStop{-kInf};
    std::vector<Order> triggerScratch;   // reused: no per-trade allocation

    double        lastTradedPrice{0.0};
    bool          hasTraded{false};
    std::uint64_t nextSequence{1};

    // ---- helpers -------------------------------------------------------------

    [[nodiscard]] RejectReason validate(const Order& order) const {
        if (order.orderQuantity == 0) return RejectReason::InvalidQuantity;
        if (!std::isfinite(order.orderPrice) || order.orderPrice <= 0.0)
            return RejectReason::InvalidPrice;
        if (order.orderType == OrderType::StopLimit &&
            (!std::isfinite(order.stopPrice) || order.stopPrice <= 0.0))
            return RejectReason::InvalidStopPrice;
        if (orderIndex.contains(order.orderID)) return RejectReason::DuplicateOrderID;
        return RejectReason::None;
    }

    [[nodiscard]] static ExecutionReport makeReport(const Order& order) {
        ExecutionReport report;
        report.orderID = order.orderID;
        report.originalQuantity = order.orderQuantity;
        report.remainingQuantity = order.orderQuantity;
        report.price = order.orderPrice;
        return report;
    }

    // A stop that arrives when the market has already reached its trigger price
    // activates immediately. With no trade history nothing can have triggered.
    [[nodiscard]] bool stopConditionMet(const Order& order) const noexcept {
        if (!hasTraded) return false;
        return order.orderSide == Side::Buy ? lastTradedPrice >= order.stopPrice
                                            : lastTradedPrice <= order.stopPrice;
    }

    // Core of addOrder(): match against the opposite side, rest the residual
    // (SR-F-02, SR-F-05, SR-F-06, SR-F-07) and fill in `report`. Used both for
    // incoming limit orders and for activated stop-limit orders.
    void executeLimit(Order& order, ExecutionReport& report, PriceRange& traded) {
        order.orderType = OrderType::Limit;   // an activated stop is now a plain limit order

        if (order.orderSide == Side::Buy) {
            matchAgainst(order, asks, report, traded,
                         [](double bestAsk, double limit) noexcept { return bestAsk <= limit; });
        } else {
            matchAgainst(order, bids, report, traded,
                         [](double bestBid, double limit) noexcept { return bestBid >= limit; });
        }

        if (order.orderQuantity > 0) restOrder(order);

        report.remainingQuantity = order.orderQuantity;
        report.executedQuantity = order.originalQuantity - order.orderQuantity;
        report.status = order.orderQuantity == 0        ? OrderStatus::FILLED
                        : report.executedQuantity > 0   ? OrderStatus::PARTIALLY_FILLED
                                                        : OrderStatus::RESTING;
    }

    template <typename Levels, typename Crosses>
    void matchAgainst(Order& incoming, Levels& opposite, ExecutionReport& report,
                      PriceRange& traded, Crosses crosses) {
        const bool incomingIsBuy = incoming.orderSide == Side::Buy;

        while (incoming.orderQuantity > 0 && !opposite.empty()) {
            const auto level = opposite.begin();
            if (!crosses(level->first, incoming.orderPrice)) break;

            auto& queue = level->second;
            while (incoming.orderQuantity > 0 && !queue.empty()) {
                Order& resting = queue.front();   // oldest first: time priority

                const std::uint32_t quantity = std::min(incoming.orderQuantity, resting.orderQuantity);
                incoming.orderQuantity -= quantity;
                resting.orderQuantity -= quantity;

                // Trades print at the resting order's price.
                const double price = resting.orderPrice;
                report.trades.push_back(Trade{
                    incomingIsBuy ? incoming.orderID : resting.orderID,
                    incomingIsBuy ? resting.orderID : incoming.orderID,
                    price, quantity});

                lastTradedPrice = price;
                hasTraded = true;
                traded.add(price);

                if (resting.orderQuantity == 0) {
                    orderIndex.erase(resting.orderID);
                    queue.pop_front();
                }
            }

            if (queue.empty()) opposite.erase(level);   // never leave an empty tier behind
        }
    }

    void restOrder(const Order& order) {
        if (order.orderSide == Side::Buy) {
            const auto level = bids.try_emplace(order.orderPrice).first;
            level->second.push_back(order);
            orderIndex.emplace(order.orderID,
                OrderLocation{.price = order.orderPrice, .side = Side::Buy, .isStop = false,
                              .listIterator = std::prev(level->second.end()),
                              .tier = OrderLocation::Tier(std::in_place_index<0>, level), .stopIndex = 0});
        } else {
            const auto level = asks.try_emplace(order.orderPrice).first;
            level->second.push_back(order);
            orderIndex.emplace(order.orderID,
                OrderLocation{.price = order.orderPrice, .side = Side::Sell, .isStop = false,
                              .listIterator = std::prev(level->second.end()),
                              .tier = OrderLocation::Tier(std::in_place_index<1>, level), .stopIndex = 0});
        }
    }

    template <std::size_t TierIndex, typename Levels>
    static Order eraseFromLevel(Levels& levels, const OrderLocation& location) {
        const auto level = std::get<TierIndex>(location.tier);
        Order removed = *location.listIterator;
        level->second.erase(location.listIterator);
        if (level->second.empty()) levels.erase(level);   // fixes the empty-tier leak
        return removed;
    }

    // ---- stop-limit queue ----------------------------------------------------

    void parkStopOrder(const Order& order) {
        auto& queue = order.orderSide == Side::Buy ? buyStopOrders : sellStopOrders;
        queue.push_back(order);
        orderIndex.emplace(order.orderID,
            OrderLocation{.price = order.orderPrice, .side = order.orderSide, .isStop = true,
                          .stopIndex = queue.size() - 1});
        if (order.orderSide == Side::Buy) minBuyStop = std::min(minBuyStop, order.stopPrice);
        else                              maxSellStop = std::max(maxSellStop, order.stopPrice);
    }

    // O(1) swap-and-pop. Erases the removed order's index entry and re-points
    // the entry of the order that was moved into its slot.
    Order removeStopAt(std::vector<Order>& queue, std::size_t position) {
        Order removed = queue[position];
        orderIndex.erase(removed.orderID);
        if (position != queue.size() - 1) {
            queue[position] = queue.back();
            orderIndex.find(queue[position].orderID)->second.stopIndex = position;
        }
        queue.pop_back();
        return removed;
    }

    // Moves every stop whose trigger condition is satisfied into triggerScratch
    // and recomputes the exact bound for the survivors.
    template <typename Triggered, typename Fold>
    void collectTriggered(std::vector<Order>& queue, double& bound, double init,
                          Triggered triggered, Fold fold) {
        double exact = init;
        for (std::size_t i = 0; i < queue.size();) {
            if (triggered(queue[i].stopPrice)) {
                triggerScratch.push_back(removeStopAt(queue, i));   // slot i now holds a new order
            } else {
                exact = fold(exact, queue[i].stopPrice);
                ++i;
            }
        }
        bound = exact;
    }

    // SR-F-04 trigger engine. `traded` is the price range of the executions
    // that just happened. Activated stops become limit orders and run through
    // the same matching path as addOrder(); the executions they cause may
    // trigger further stops, so we iterate in waves until the book is quiet.
    // Iterative (not recursive) so deep cascades cannot overflow the stack.
    void evaluateStopOrders(PriceRange traded, ExecutionReport& report) {
        while (traded.valid) {
            triggerScratch.clear();

            // A price path that touched [low, high] satisfies a buy stop when
            // high >= stopPrice and a sell stop when low <= stopPrice.
            if (!buyStopOrders.empty() && minBuyStop <= traded.high) {
                collectTriggered(buyStopOrders, minBuyStop, kInf,
                                 [&](double stop) noexcept { return stop <= traded.high; },
                                 [](double a, double b) noexcept { return std::min(a, b); });
            }
            if (!sellStopOrders.empty() && maxSellStop >= traded.low) {
                collectTriggered(sellStopOrders, maxSellStop, -kInf,
                                 [&](double stop) noexcept { return stop >= traded.low; },
                                 [](double a, double b) noexcept { return std::max(a, b); });
            }
            if (triggerScratch.empty()) break;

            // Preserve time priority between stops that fire together.
            std::ranges::sort(triggerScratch, {}, &Order::sequence);

            PriceRange next;
            for (Order& stop : triggerScratch) {
                ExecutionReport triggeredReport = makeReport(stop);
                executeLimit(stop, triggeredReport, next);
                triggeredReport.triggered = true;
                report.triggeredOrders.push_back(std::move(triggeredReport));
            }
            traded = next;
        }
    }
};
