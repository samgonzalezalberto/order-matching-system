// Unit, integration and benchmark tests for the order matching engine.
//
//   OrderBookPriceTime   SR-F-02, SR-F-05      price-time priority
//   OrderBookPartial     SR-F-06, SR-F-07      partial fills, residual allocation
//   OrderBookValidation                        rejects
//   OrderBookCancel      SR-F-08, SR-F-09      cancellation, tier cleanup, O(1)
//   StopLimit            SR-F-03, SR-F-04      registration, triggers, cascades
//   MatchingEngineConcurrency  SR-N-01         invariants under contention
//   ApiHandlers / ApiHttp      SR-I-01..03     REST contract
//   Benchmark*           SR-N-01, SR-N-02      throughput and latency

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "api_server.hpp"
#include "matching_engine.hpp"
#include "order_book.hpp"

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// ---- helpers ----------------------------------------------------------------

Order buy(std::uint64_t id, double price, std::uint32_t qty) { return Order::limit(id, Side::Buy, price, qty); }
Order sell(std::uint64_t id, double price, std::uint32_t qty) { return Order::limit(id, Side::Sell, price, qty); }
Order buyStop(std::uint64_t id, double stop, double price, std::uint32_t qty) {
    return Order::stopLimit(id, Side::Buy, stop, price, qty);
}
Order sellStop(std::uint64_t id, double stop, double price, std::uint32_t qty) {
    return Order::stopLimit(id, Side::Sell, stop, price, qty);
}

std::vector<std::uint64_t> counterpartyIDs(const ExecutionReport& r, Side incoming) {
    std::vector<std::uint64_t> ids;
    for (const Trade& t : r.trades) ids.push_back(incoming == Side::Buy ? t.sellOrderID : t.buyOrderID);
    return ids;
}

// ===========================================================================
// Price-Time priority (SR-F-02, SR-F-05)
// ===========================================================================

TEST(OrderBookPriceTime, FifoWithinSamePriceTier) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    book.addOrder(sell(2, 100.0, 10));
    book.addOrder(sell(3, 100.0, 10));

    const ExecutionReport r = book.addOrder(buy(4, 100.0, 25));

    ASSERT_EQ(r.trades.size(), 3u);
    EXPECT_EQ(counterpartyIDs(r, Side::Buy), (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(r.trades[0].tradedQuantity, 10u);
    EXPECT_EQ(r.trades[1].tradedQuantity, 10u);
    EXPECT_EQ(r.trades[2].tradedQuantity, 5u);
    EXPECT_EQ(r.status, OrderStatus::FILLED);

    // Order 3 keeps the unfilled 5 and is the only thing left.
    EXPECT_TRUE(book.contains(3));
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.stats().askQuantity, 5u);
}

TEST(OrderBookPriceTime, FifoOnTheBidSide) {
    OrderBook book;
    book.addOrder(buy(1, 50.0, 5));
    book.addOrder(buy(2, 50.0, 5));
    const ExecutionReport r = book.addOrder(sell(3, 50.0, 7));
    EXPECT_EQ(counterpartyIDs(r, Side::Sell), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(r.trades[0].tradedQuantity, 5u);
    EXPECT_EQ(r.trades[1].tradedQuantity, 2u);
}

TEST(OrderBookPriceTime, LowerAskPriceMatchesFirstRegardlessOfArrival) {
    OrderBook book;
    book.addOrder(sell(1, 102.0, 10));   // earliest but worst price
    book.addOrder(sell(2, 100.0, 10));
    book.addOrder(sell(3, 101.0, 10));

    const ExecutionReport r = book.addOrder(buy(4, 102.0, 30));
    EXPECT_EQ(counterpartyIDs(r, Side::Buy), (std::vector<std::uint64_t>{2, 3, 1}));
}

TEST(OrderBookPriceTime, HigherBidPriceMatchesFirstRegardlessOfArrival) {
    OrderBook book;
    book.addOrder(buy(1, 98.0, 10));
    book.addOrder(buy(2, 100.0, 10));
    book.addOrder(buy(3, 99.0, 10));

    const ExecutionReport r = book.addOrder(sell(4, 98.0, 30));
    EXPECT_EQ(counterpartyIDs(r, Side::Sell), (std::vector<std::uint64_t>{2, 3, 1}));
}

TEST(OrderBookPriceTime, TradesPrintAtRestingOrderPrice) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    const ExecutionReport r = book.addOrder(buy(2, 105.0, 10));   // willing to pay 105
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_DOUBLE_EQ(r.trades[0].tradedPrice, 100.0);
    EXPECT_EQ(r.trades[0].buyOrderID, 2u);
    EXPECT_EQ(r.trades[0].sellOrderID, 1u);
}

TEST(OrderBookPriceTime, NonCrossingOrderRests) {
    OrderBook book;
    book.addOrder(sell(1, 101.0, 10));
    const ExecutionReport r = book.addOrder(buy(2, 100.0, 10));
    EXPECT_EQ(r.status, OrderStatus::RESTING);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(r.remainingQuantity, 10u);
    EXPECT_EQ(r.executedQuantity, 0u);
    EXPECT_DOUBLE_EQ(*book.bestBid(), 100.0);
    EXPECT_DOUBLE_EQ(*book.bestAsk(), 101.0);
}

TEST(OrderBookPriceTime, LimitPriceIsNeverExceeded) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    book.addOrder(sell(2, 103.0, 10));
    const ExecutionReport r = book.addOrder(buy(3, 101.0, 20));
    EXPECT_EQ(r.executedQuantity, 10u);
    EXPECT_EQ(r.status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_DOUBLE_EQ(*book.bestBid(), 101.0);   // residual rests at the limit
    EXPECT_DOUBLE_EQ(*book.bestAsk(), 103.0);
}

// ===========================================================================
// Partial fills & residual allocation (SR-F-06, SR-F-07)
// ===========================================================================

TEST(OrderBookPartial, IncomingPartialFillRestsResidual) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 60));
    const ExecutionReport r = book.addOrder(buy(2, 100.0, 100));

    EXPECT_EQ(r.status, OrderStatus::PARTIALLY_FILLED);
    EXPECT_EQ(r.originalQuantity, 100u);
    EXPECT_EQ(r.executedQuantity, 60u);
    EXPECT_EQ(r.remainingQuantity, 40u);
    EXPECT_TRUE(book.contains(2));
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.stats().bidQuantity, 40u);
}

TEST(OrderBookPartial, ResidualJoinsBackOfTier) {
    OrderBook book;
    book.addOrder(sell(10, 100.0, 60));
    book.addOrder(buy(1, 100.0, 100));   // 60 trade, 40 rest
    book.addOrder(buy(2, 100.0, 10));    // queues behind order 1

    const ExecutionReport r = book.addOrder(sell(11, 100.0, 45));
    EXPECT_EQ(counterpartyIDs(r, Side::Sell), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(r.trades[0].tradedQuantity, 40u);
    EXPECT_EQ(r.trades[1].tradedQuantity, 5u);
}

TEST(OrderBookPartial, RestingOrderIsReducedAndKeepsItsPlace) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    book.addOrder(sell(2, 100.0, 10));
    book.addOrder(buy(3, 100.0, 4));   // takes 4 from order 1

    // Order 1 is still first in line, now with 6 left.
    const ExecutionReport r = book.addOrder(buy(4, 100.0, 8));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].sellOrderID, 1u);
    EXPECT_EQ(r.trades[0].tradedQuantity, 6u);
    EXPECT_EQ(r.trades[1].sellOrderID, 2u);
    EXPECT_EQ(r.trades[1].tradedQuantity, 2u);
}

TEST(OrderBookPartial, ExactFillLeavesNothingBehind) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    const ExecutionReport r = book.addOrder(buy(2, 100.0, 10));
    EXPECT_EQ(r.status, OrderStatus::FILLED);
    EXPECT_EQ(r.remainingQuantity, 0u);
    EXPECT_EQ(book.activeOrderCount(), 0u);
    EXPECT_EQ(book.bidLevelCount(), 0u);
    EXPECT_EQ(book.askLevelCount(), 0u);
}

TEST(OrderBookPartial, SweepsMultipleLevels) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 5));
    book.addOrder(sell(2, 101.0, 5));
    book.addOrder(sell(3, 102.0, 5));
    const ExecutionReport r = book.addOrder(buy(4, 102.0, 12));
    EXPECT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_EQ(r.trades.size(), 3u);
    EXPECT_DOUBLE_EQ(r.trades[0].tradedPrice, 100.0);
    EXPECT_DOUBLE_EQ(r.trades[1].tradedPrice, 101.0);
    EXPECT_DOUBLE_EQ(r.trades[2].tradedPrice, 102.0);
    EXPECT_EQ(book.askLevelCount(), 1u);   // 102 still has 3 left
    EXPECT_DOUBLE_EQ(*book.lastPrice(), 102.0);
}

// ===========================================================================
// Validation
// ===========================================================================

TEST(OrderBookValidation, RejectsDuplicateActiveOrderID) {
    OrderBook book;
    book.addOrder(buy(1, 100.0, 10));
    const ExecutionReport r = book.addOrder(sell(1, 200.0, 10));
    EXPECT_EQ(r.status, OrderStatus::REJECTED);
    EXPECT_EQ(r.reason, RejectReason::DuplicateOrderID);
    EXPECT_EQ(book.activeOrderCount(), 1u);
}

TEST(OrderBookValidation, AllowsIDReuseAfterOrderLeavesTheBook) {
    OrderBook book;
    book.addOrder(buy(1, 100.0, 10));
    (void)book.cancelOrder(1);
    EXPECT_EQ(book.addOrder(buy(1, 100.0, 10)).status, OrderStatus::RESTING);
}

// SR-F-01 decision: orderIDs are unique among ACTIVE orders (the scope SR-D-02
// indexes). Lifetime uniqueness would need an unbounded history of IDs, which
// is incompatible with bounded memory in a non-persistent engine (SR-D-03);
// uniqueness across time is the upstream gateway's responsibility (SR-N-03).
TEST(OrderBookValidation, FullyFilledOrderIDCanBeReused) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 5));
    EXPECT_EQ(book.addOrder(buy(2, 100.0, 5)).status, OrderStatus::FILLED);
    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.addOrder(buy(1, 99.0, 5)).status, OrderStatus::RESTING);   // 1 was fully filled earlier
    EXPECT_EQ(book.activeOrderCount(), 1u);
}

TEST(OrderBookValidation, RejectedOrderDoesNotReserveItsID) {
    OrderBook book;
    EXPECT_EQ(book.addOrder(buy(1, 100.0, 0)).status, OrderStatus::REJECTED);
    EXPECT_EQ(book.addOrder(buy(1, 100.0, 5)).status, OrderStatus::RESTING);
}

TEST(OrderBookValidation, RejectsBadQuantityAndPrices) {
    OrderBook book;
    EXPECT_EQ(book.addOrder(buy(1, 100.0, 0)).reason, RejectReason::InvalidQuantity);
    EXPECT_EQ(book.addOrder(buy(2, 0.0, 5)).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(book.addOrder(buy(3, -1.0, 5)).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(book.addOrder(buy(4, std::numeric_limits<double>::quiet_NaN(), 5)).reason,
              RejectReason::InvalidPrice);
    EXPECT_EQ(book.addOrder(buyStop(5, 0.0, 100.0, 5)).reason, RejectReason::InvalidStopPrice);
    EXPECT_EQ(book.activeOrderCount(), 0u);
}

// ===========================================================================
// Cancellation (SR-F-08, SR-F-09) and tier cleanup
// ===========================================================================

TEST(OrderBookCancel, ReportsCancelledWithRemainingQuantity) {
    OrderBook book;
    book.addOrder(buy(1, 100.0, 40));
    const ExecutionReport r = book.cancelOrder(1);
    EXPECT_EQ(r.status, OrderStatus::CANCELLED);
    EXPECT_EQ(r.orderID, 1u);
    EXPECT_EQ(r.remainingQuantity, 40u);
    EXPECT_EQ(r.executedQuantity, 0u);
    EXPECT_FALSE(book.contains(1));
}

TEST(OrderBookCancel, PartiallyFilledOrderReportsExecutedAndRemaining) {
    OrderBook book;
    book.addOrder(buy(1, 100.0, 100));
    book.addOrder(sell(2, 100.0, 60));
    const ExecutionReport r = book.cancelOrder(1);
    EXPECT_EQ(r.status, OrderStatus::CANCELLED);
    EXPECT_EQ(r.originalQuantity, 100u);
    EXPECT_EQ(r.executedQuantity, 60u);
    EXPECT_EQ(r.remainingQuantity, 40u);
}

TEST(OrderBookCancel, UnknownAndRepeatedCancelsAreRejected) {
    OrderBook book;
    EXPECT_EQ(book.cancelOrder(42).reason, RejectReason::UnknownOrder);
    book.addOrder(buy(1, 100.0, 10));
    EXPECT_EQ(book.cancelOrder(1).status, OrderStatus::CANCELLED);
    const ExecutionReport again = book.cancelOrder(1);
    EXPECT_EQ(again.status, OrderStatus::REJECTED);
    EXPECT_EQ(again.reason, RejectReason::UnknownOrder);
}

TEST(OrderBookCancel, CancelledOrderCannotBeMatched) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 10));
    book.addOrder(sell(2, 100.0, 10));
    (void)book.cancelOrder(1);
    const ExecutionReport r = book.addOrder(buy(3, 100.0, 10));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].sellOrderID, 2u);
}

TEST(OrderBookCancel, CancellingMiddleOfTierPreservesNeighbourOrder) {
    OrderBook book;
    for (std::uint64_t id = 1; id <= 5; ++id) book.addOrder(sell(id, 100.0, 1));
    (void)book.cancelOrder(3);
    const ExecutionReport r = book.addOrder(buy(9, 100.0, 4));
    EXPECT_EQ(counterpartyIDs(r, Side::Buy), (std::vector<std::uint64_t>{1, 2, 4, 5}));
}

TEST(OrderBookCancel, ErasesEmptyBidTier) {
    OrderBook book;
    book.addOrder(buy(1, 100.0, 10));
    book.addOrder(buy(2, 100.0, 10));
    book.addOrder(buy(3, 99.0, 10));
    ASSERT_EQ(book.bidLevelCount(), 2u);

    (void)book.cancelOrder(1);
    EXPECT_EQ(book.bidLevelCount(), 2u);   // order 2 still occupies the 100 tier
    (void)book.cancelOrder(2);
    EXPECT_EQ(book.bidLevelCount(), 1u);   // last order out => key erased
    EXPECT_DOUBLE_EQ(*book.bestBid(), 99.0);
    (void)book.cancelOrder(3);
    EXPECT_EQ(book.bidLevelCount(), 0u);
    EXPECT_FALSE(book.bestBid().has_value());
}

TEST(OrderBookCancel, ErasesEmptyAskTier) {
    OrderBook book;
    book.addOrder(sell(1, 101.0, 10));
    book.addOrder(sell(2, 102.0, 10));
    (void)book.cancelOrder(1);
    EXPECT_EQ(book.askLevelCount(), 1u);
    EXPECT_DOUBLE_EQ(*book.bestAsk(), 102.0);
    (void)book.cancelOrder(2);
    EXPECT_EQ(book.askLevelCount(), 0u);
}

TEST(OrderBookCancel, NoTierLeakAfterHeavyAddCancelChurn) {
    OrderBook book;
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> live;
    std::uint64_t nextID = 1;
    for (int i = 0; i < 20000; ++i) {
        // Non-crossing: bids below 100, asks above 100, so nothing matches.
        const bool isBuy = rng() & 1;
        const double price = isBuy ? 90.0 + static_cast<double>(rng() % 900) / 100.0
                                   : 101.0 + static_cast<double>(rng() % 900) / 100.0;
        const auto id = nextID++;
        book.addOrder(isBuy ? buy(id, price, 1) : sell(id, price, 1));
        live.push_back(id);
    }
    std::ranges::shuffle(live, rng);
    for (const auto id : live) ASSERT_EQ(book.cancelOrder(id).status, OrderStatus::CANCELLED);

    EXPECT_EQ(book.bidLevelCount(), 0u);
    EXPECT_EQ(book.askLevelCount(), 0u);
    EXPECT_EQ(book.activeOrderCount(), 0u);
}

TEST(OrderBookCancel, MatchingAlsoClearsExhaustedTiers) {
    OrderBook book;
    book.addOrder(sell(1, 100.0, 5));
    book.addOrder(sell(2, 101.0, 5));
    (void)book.addOrder(buy(3, 101.0, 10));
    EXPECT_EQ(book.askLevelCount(), 0u);
    EXPECT_EQ(book.bidLevelCount(), 0u);
}

// Average cost per cancel must not grow with the number of resting orders.
// Every order sits in ONE price tier, so any implementation that scanned the
// FIFO list (or the tier) to find an order would be ~O(n): cancelling from a
// 100x larger book would be ~100x slower per operation. A constant-time lookup
// only suffers cache effects.
double cancelNanosPerOp(std::size_t bookSize, std::size_t cancels) {
    OrderBook book(bookSize * 2);
    for (std::uint64_t id = 1; id <= bookSize; ++id) book.addOrder(buy(id, 100.0, 1));

    std::vector<std::uint64_t> ids(bookSize);
    std::iota(ids.begin(), ids.end(), 1);
    std::mt19937_64 rng(123);
    std::ranges::shuffle(ids, rng);
    ids.resize(cancels);

    const auto start = Clock::now();
    std::size_t cancelled = 0;
    for (const auto id : ids) cancelled += book.cancelOrder(id).status == OrderStatus::CANCELLED;
    const auto elapsed = Clock::now() - start;

    EXPECT_EQ(cancelled, cancels);
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
           static_cast<double>(cancels);
}

TEST(OrderBookCancel, CancellationCostIsIndependentOfBookSize) {
    // 100x size difference: an O(n) cancel would be >= ~100x slower per op,
    // while an O(1) one only pays for cache misses in the larger book (~10x).
    constexpr std::size_t kSmall = 2'000, kLarge = 200'000, kCancels = 2'000;
    double small = 1e18, large = 1e18;
    for (int attempt = 0; attempt < 5; ++attempt) {   // best-of-5 to damp scheduler noise
        small = std::min(small, cancelNanosPerOp(kSmall, kCancels));
        large = std::min(large, cancelNanosPerOp(kLarge, kCancels));
    }
    std::cout << "[ INFO     ] cancel ns/op: book=" << kSmall << " -> " << std::fixed << std::setprecision(1)
              << small << ", book=" << kLarge << " -> " << large << " (ratio " << large / small << ")\n";
    EXPECT_LT(large / small, 40.0) << "cancel cost scaled with book size; expected O(1)";
    EXPECT_LT(large, 5'000.0) << "an O(1) cancel should cost well under 5 us even in a 200k-order book";
}

// ===========================================================================
// Stop-limit orders (SR-F-03, SR-F-04)
// ===========================================================================

// Prints a trade at `price` so that lastTradedPrice == price.
void printTrade(OrderBook& book, double price, std::uint64_t idA, std::uint64_t idB) {
    book.addOrder(sell(idA, price, 1));
    book.addOrder(buy(idB, price, 1));
}

TEST(StopLimit, RegistersInSecondaryQueueNotInTheBook) {
    OrderBook book;
    const ExecutionReport r = book.addOrder(buyStop(1, 105.0, 106.0, 10));
    EXPECT_EQ(r.status, OrderStatus::RESTING);
    EXPECT_EQ(r.remainingQuantity, 10u);
    EXPECT_EQ(book.stopOrderCount(), 1u);
    EXPECT_EQ(book.bidLevelCount(), 0u);
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_TRUE(book.contains(1));   // indexed for O(1) cancel

    // Stops are not visible liquidity: a sell at the stop's limit does not match it.
    const ExecutionReport s = book.addOrder(sell(2, 106.0, 10));
    EXPECT_TRUE(s.trades.empty());
}

TEST(StopLimit, BuyStopTriggersWhenLastPriceReachesStopPrice) {
    OrderBook book;
    book.addOrder(sell(1, 101.0, 10));
    ASSERT_EQ(book.addOrder(buyStop(2, 100.0, 102.0, 5)).status, OrderStatus::RESTING);

    // lastTradedPrice == stopPrice exactly => triggers ("reaches").
    book.addOrder(sell(3, 100.0, 1));
    const ExecutionReport aggressor = book.addOrder(buy(4, 100.0, 1));

    EXPECT_EQ(aggressor.status, OrderStatus::FILLED);
    ASSERT_EQ(aggressor.triggeredOrders.size(), 1u);
    const ExecutionReport& triggered = aggressor.triggeredOrders[0];
    EXPECT_EQ(triggered.orderID, 2u);
    EXPECT_EQ(triggered.status, OrderStatus::FILLED);
    ASSERT_EQ(triggered.trades.size(), 1u);
    EXPECT_EQ(triggered.trades[0].sellOrderID, 1u);
    EXPECT_DOUBLE_EQ(triggered.trades[0].tradedPrice, 101.0);

    EXPECT_EQ(book.stopOrderCount(), 0u);
    EXPECT_FALSE(book.contains(2));
    EXPECT_DOUBLE_EQ(*book.lastPrice(), 101.0);
}

TEST(StopLimit, BuyStopStaysDormantBelowItsTrigger) {
    OrderBook book;
    book.addOrder(buyStop(1, 101.0, 102.0, 5));
    printTrade(book, 100.0, 10, 11);
    EXPECT_EQ(book.stopOrderCount(), 1u);
    EXPECT_TRUE(book.contains(1));
}

TEST(StopLimit, SellStopTriggersWhenLastPriceFallsToStopPrice) {
    OrderBook book;
    book.addOrder(buy(1, 99.0, 10));
    ASSERT_EQ(book.addOrder(sellStop(2, 100.0, 98.0, 5)).status, OrderStatus::RESTING);

    book.addOrder(sell(3, 100.0, 1));
    const ExecutionReport aggressor = book.addOrder(buy(4, 100.0, 1));   // last = 100 <= 100

    ASSERT_EQ(aggressor.triggeredOrders.size(), 1u);
    const ExecutionReport& triggered = aggressor.triggeredOrders[0];
    EXPECT_EQ(triggered.orderID, 2u);
    EXPECT_EQ(triggered.status, OrderStatus::FILLED);
    ASSERT_EQ(triggered.trades.size(), 1u);
    EXPECT_EQ(triggered.trades[0].buyOrderID, 1u);
    EXPECT_DOUBLE_EQ(triggered.trades[0].tradedPrice, 99.0);
}

TEST(StopLimit, SellStopStaysDormantAboveItsTrigger) {
    OrderBook book;
    book.addOrder(sellStop(1, 99.0, 98.0, 5));
    printTrade(book, 100.0, 10, 11);
    EXPECT_EQ(book.stopOrderCount(), 1u);
}

TEST(StopLimit, NoTradeHistoryMeansNothingTriggers) {
    OrderBook book;
    // A sell stop would satisfy 0 <= stop if "no trade" were treated as price 0.
    EXPECT_EQ(book.addOrder(sellStop(1, 100.0, 99.0, 5)).status, OrderStatus::RESTING);
    EXPECT_EQ(book.stopOrderCount(), 1u);
}

TEST(StopLimit, ActivatesImmediatelyIfMarketAlreadyThroughStopPrice) {
    OrderBook book;
    printTrade(book, 100.0, 10, 11);   // last = 100
    book.addOrder(buy(12, 99.0, 10));

    // Sell stop at 105: last (100) <= 105 already, so it activates on arrival.
    const ExecutionReport r = book.addOrder(sellStop(1, 105.0, 99.0, 4));
    EXPECT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].buyOrderID, 12u);
    EXPECT_EQ(book.stopOrderCount(), 0u);
}

TEST(StopLimit, TriggeredStopThatCannotFillRestsAsLimitOrder) {
    OrderBook book;
    book.addOrder(buyStop(1, 100.0, 99.0, 3));
    printTrade(book, 100.0, 10, 11);

    EXPECT_EQ(book.stopOrderCount(), 0u);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_DOUBLE_EQ(*book.bestBid(), 99.0);   // now a plain limit order in the book
    EXPECT_TRUE(book.contains(1));

    // ...and it behaves like one: it can be matched.
    const ExecutionReport r = book.addOrder(sell(20, 99.0, 3));
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].buyOrderID, 1u);
}

// SR-I-03: only the four SRS statuses appear in successful responses; stop
// activation is signalled through the separate `triggered` flag.
TEST(StopLimit, ActivatedStopThatOnlyRestsReportsRestingWithTriggeredFlag) {
    OrderBook book;
    EXPECT_FALSE(book.addOrder(buyStop(1, 100.0, 99.0, 3)).triggered);   // parked, not yet activated
    book.addOrder(sell(10, 100.0, 1));
    const ExecutionReport r = book.addOrder(buy(11, 100.0, 1));
    ASSERT_EQ(r.triggeredOrders.size(), 1u);
    EXPECT_EQ(r.triggeredOrders[0].status, OrderStatus::RESTING);
    EXPECT_TRUE(r.triggeredOrders[0].triggered);
    EXPECT_EQ(r.triggeredOrders[0].remainingQuantity, 3u);
    EXPECT_FALSE(r.triggered);   // the aggressor itself was a plain limit order
}

TEST(StopLimit, StopActivatedOnArrivalIsFlaggedTriggered) {
    OrderBook book;
    printTrade(book, 100.0, 10, 11);
    const ExecutionReport r = book.addOrder(buyStop(1, 100.0, 99.0, 3));
    EXPECT_EQ(r.status, OrderStatus::RESTING);
    EXPECT_TRUE(r.triggered);
}

TEST(StopLimit, MultiStageBuyCascade) {
    OrderBook book;
    for (std::uint64_t i = 0; i < 4; ++i) book.addOrder(sell(1 + i, 101.0 + static_cast<double>(i), 10));
    // Each stop's execution price becomes the next stop's trigger.
    book.addOrder(buyStop(11, 101.0, 102.0, 10));
    book.addOrder(buyStop(12, 102.0, 103.0, 10));
    book.addOrder(buyStop(13, 103.0, 104.0, 10));
    ASSERT_EQ(book.stopOrderCount(), 3u);

    const ExecutionReport r = book.addOrder(buy(20, 101.0, 10));   // one trade at 101

    EXPECT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_EQ(r.triggeredOrders.size(), 3u);
    const std::uint64_t expectedIDs[] = {11, 12, 13};
    const double expectedPrices[] = {102.0, 103.0, 104.0};
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(r.triggeredOrders[i].orderID, expectedIDs[i]);
        EXPECT_EQ(r.triggeredOrders[i].status, OrderStatus::FILLED);
        ASSERT_EQ(r.triggeredOrders[i].trades.size(), 1u);
        EXPECT_DOUBLE_EQ(r.triggeredOrders[i].trades[0].tradedPrice, expectedPrices[i]);
    }
    EXPECT_DOUBLE_EQ(*book.lastPrice(), 104.0);
    EXPECT_EQ(book.stopOrderCount(), 0u);
    EXPECT_EQ(book.activeOrderCount(), 0u);
    EXPECT_EQ(book.askLevelCount(), 0u);
}

TEST(StopLimit, MultiStageSellCascade) {
    OrderBook book;
    for (std::uint64_t i = 0; i < 4; ++i) book.addOrder(buy(1 + i, 99.0 - static_cast<double>(i), 10));
    book.addOrder(sellStop(11, 99.0, 98.0, 10));
    book.addOrder(sellStop(12, 98.0, 97.0, 10));
    book.addOrder(sellStop(13, 97.0, 96.0, 10));

    const ExecutionReport r = book.addOrder(sell(20, 99.0, 10));

    ASSERT_EQ(r.triggeredOrders.size(), 3u);
    EXPECT_EQ(r.triggeredOrders[0].orderID, 11u);
    EXPECT_EQ(r.triggeredOrders[1].orderID, 12u);
    EXPECT_EQ(r.triggeredOrders[2].orderID, 13u);
    EXPECT_DOUBLE_EQ(*book.lastPrice(), 96.0);
    EXPECT_EQ(book.activeOrderCount(), 0u);
}

TEST(StopLimit, CascadeCanCrossSides) {
    // One trade at 100 satisfies both a buy stop (last >= 100) and a sell stop
    // (last <= 101); they must fire in arrival order regardless of side.
    OrderBook book;
    book.addOrder(sell(1, 101.0, 10));
    book.addOrder(buy(2, 100.0, 10));
    book.addOrder(buyStop(11, 100.0, 101.0, 10));     // fires at 100
    book.addOrder(sellStop(12, 101.0, 100.0, 10));    // fires when last <= 101
    // Trade at 100 (sell 3 vs resting buy 2): both stops are eligible, in arrival order.
    const ExecutionReport r = book.addOrder(sell(3, 100.0, 1));
    ASSERT_EQ(r.triggeredOrders.size(), 2u);
    EXPECT_EQ(r.triggeredOrders[0].orderID, 11u);   // earlier sequence first
    EXPECT_EQ(r.triggeredOrders[1].orderID, 12u);
    EXPECT_EQ(book.stopOrderCount(), 0u);
}

TEST(StopLimit, SimultaneousTriggersActivateInTimePriority) {
    OrderBook book;
    book.addOrder(buyStop(11, 100.0, 105.0, 10));
    book.addOrder(buyStop(12, 100.0, 105.0, 10));
    book.addOrder(buyStop(13, 100.0, 105.0, 10));
    (void)book.cancelOrder(11);   // swap-and-pop puts 13 ahead of 12 in the vector

    book.addOrder(sell(20, 101.0, 10));   // liquidity for exactly one stop
    book.addOrder(sell(21, 100.0, 1));
    const ExecutionReport r = book.addOrder(buy(22, 100.0, 1));   // trade at 100 fires 12 and 13

    ASSERT_EQ(r.triggeredOrders.size(), 2u);
    EXPECT_EQ(r.triggeredOrders[0].orderID, 12u);
    EXPECT_EQ(r.triggeredOrders[0].status, OrderStatus::FILLED);
    EXPECT_EQ(r.triggeredOrders[1].orderID, 13u);
    EXPECT_EQ(r.triggeredOrders[1].status, OrderStatus::RESTING);     // no liquidity left
    EXPECT_TRUE(r.triggeredOrders[1].triggered);
}

TEST(StopLimit, CancelStopOrderKeepsIndexConsistent) {
    OrderBook book;
    book.addOrder(buyStop(11, 100.0, 101.0, 10));
    book.addOrder(sellStop(12, 90.0, 89.0, 20));
    book.addOrder(buyStop(13, 100.0, 101.0, 30));
    book.addOrder(buyStop(14, 100.0, 101.0, 40));
    ASSERT_EQ(book.stopOrderCount(), 4u);

    const ExecutionReport first = book.cancelOrder(11);   // 14 is swapped into slot 0
    EXPECT_EQ(first.status, OrderStatus::CANCELLED);
    EXPECT_EQ(first.remainingQuantity, 10u);
    EXPECT_FALSE(book.contains(11));

    EXPECT_EQ(book.cancelOrder(14).remainingQuantity, 40u);   // the moved order is still reachable
    EXPECT_EQ(book.cancelOrder(13).remainingQuantity, 30u);
    EXPECT_EQ(book.cancelOrder(12).remainingQuantity, 20u);
    EXPECT_EQ(book.stopOrderCount(), 0u);
    EXPECT_EQ(book.activeOrderCount(), 0u);
    EXPECT_EQ(book.cancelOrder(12).reason, RejectReason::UnknownOrder);
}

TEST(StopLimit, CancelledStopNeverTriggers) {
    OrderBook book;
    book.addOrder(sell(1, 101.0, 10));
    book.addOrder(buyStop(2, 100.0, 102.0, 10));
    (void)book.cancelOrder(2);
    book.addOrder(sell(3, 100.0, 1));
    const ExecutionReport r = book.addOrder(buy(4, 100.0, 1));
    EXPECT_TRUE(r.triggeredOrders.empty());
    EXPECT_EQ(book.stats().askQuantity, 10u);   // order 1 untouched
}

TEST(StopLimit, DuplicateIDAcrossStopAndLimitIsRejected) {
    OrderBook book;
    book.addOrder(buyStop(1, 100.0, 101.0, 10));
    EXPECT_EQ(book.addOrder(buy(1, 50.0, 1)).reason, RejectReason::DuplicateOrderID);
}

// Randomised mix of limit orders, stop-limit orders and cancels. Invariants:
//   * a settled book is never crossed (bestBid < bestAsk), after every operation;
//   * the index covers exactly the book + the stop queues;
//   * exact conservation: every accepted unit is either traded (consuming one
//     buy unit and one sell unit) or still outstanding when finally cancelled.
TEST(StopLimit, RandomisedWorkloadPreservesInvariants) {
    OrderBook book;
    std::mt19937_64 rng(2024);
    std::vector<std::uint64_t> ids;
    std::uint64_t accepted = 0, traded = 0, cancelledQty = 0, nextID = 1;
    std::size_t triggeredSeen = 0;

    auto tally = [&](const ExecutionReport& r) {
        for (const Trade& t : r.trades) traded += t.tradedQuantity;
        for (const ExecutionReport& t : r.triggeredOrders) {
            ++triggeredSeen;
            for (const Trade& trade : t.trades) traded += trade.tradedQuantity;
        }
    };

    for (int i = 0; i < 30'000; ++i) {
        const auto roll = rng() % 10;
        if (roll < 2 && !ids.empty()) {
            // The target may already be gone (filled/cancelled): both outcomes are legal.
            const ExecutionReport c = book.cancelOrder(ids[rng() % ids.size()]);
            if (c.status == OrderStatus::CANCELLED) cancelledQty += c.remainingQuantity;
        } else {
            const auto id = nextID++;
            const bool isBuy = rng() & 1;
            const auto qty = static_cast<std::uint32_t>(1 + rng() % 20);
            const double px = 95.0 + static_cast<double>(rng() % 101) / 10.0;     // 95.0 .. 105.0
            const double stop = 95.0 + static_cast<double>(rng() % 101) / 10.0;
            const Order order = roll < 4 ? Order::stopLimit(id, isBuy ? Side::Buy : Side::Sell, stop, px, qty)
                                         : Order::limit(id, isBuy ? Side::Buy : Side::Sell, px, qty);
            const ExecutionReport r = book.addOrder(order);
            ASSERT_NE(r.status, OrderStatus::REJECTED);
            accepted += qty;
            ids.push_back(id);
            tally(r);
        }
        if (const auto bid = book.bestBid(), ask = book.bestAsk(); bid && ask) {
            ASSERT_LT(*bid, *ask) << "crossed book after op " << i;
        }
        ASSERT_EQ(book.activeOrderCount(), book.stats().restingOrders + book.stopOrderCount());
    }
    EXPECT_GT(triggeredSeen, 100u) << "workload should exercise the trigger engine";

    for (const auto id : ids) {   // drain whatever is still live
        const ExecutionReport r = book.cancelOrder(id);
        if (r.status == OrderStatus::CANCELLED) cancelledQty += r.remainingQuantity;
    }
    EXPECT_EQ(accepted, cancelledQty + 2 * traded);
    EXPECT_EQ(book.activeOrderCount(), 0u);
    EXPECT_EQ(book.bidLevelCount() + book.askLevelCount() + book.stopOrderCount(), 0u);
}

// ===========================================================================
// Thread safety (SR-N-01)
// ===========================================================================

TEST(MatchingEngineConcurrency, ConservationAndConsistencyUnderContention) {
    constexpr int kThreads = 8;
    constexpr int kOrdersPerThread = 20'000;
    MatchingEngine engine;

    std::atomic<std::uint64_t> boughtQty{0}, soldQty{0}, tradedQty{0}, rejects{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937_64 rng(1000 + static_cast<unsigned>(t));
            std::uint64_t myBought = 0, mySold = 0, myTraded = 0, myRejects = 0;
            for (int i = 0; i < kOrdersPerThread; ++i) {
                const std::uint64_t id = static_cast<std::uint64_t>(t + 1) * 1'000'000 + static_cast<unsigned>(i);
                const bool isBuy = rng() & 1;
                const double price = 100.0 + static_cast<double>(rng() % 20) / 100.0;
                const auto qty = static_cast<std::uint32_t>(1 + rng() % 10);
                const ExecutionReport r = engine.submit(isBuy ? buy(id, price, qty) : sell(id, price, qty));
                if (r.status == OrderStatus::REJECTED) { ++myRejects; continue; }
                (isBuy ? myBought : mySold) += qty;
                for (const Trade& trade : r.trades) myTraded += trade.tradedQuantity;
            }
            boughtQty += myBought; soldQty += mySold; tradedQty += myTraded; rejects += myRejects;
        });
    }
    for (auto& w : workers) w.join();

    const BookStats stats = engine.stats();
    EXPECT_EQ(rejects.load(), 0u);
    // Every traded unit left exactly one buyer and one seller.
    EXPECT_EQ(boughtQty.load() - stats.bidQuantity, tradedQty.load());
    EXPECT_EQ(soldQty.load() - stats.askQuantity, tradedQty.load());
    // A settled book is never crossed.
    EXPECT_TRUE(stats.bidQuantity == 0 || stats.askQuantity == 0 || *stats.bestBid < *stats.bestAsk);
}

TEST(MatchingEngineConcurrency, ConcurrentSubmitAndCancelOfSameOrderHasOneWinner) {
    MatchingEngine engine;
    for (std::uint64_t id = 1; id <= 5000; ++id) (void)engine.submit(buy(id, 100.0, 1));

    std::atomic<int> cancelled{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (std::uint64_t id = 1; id <= 5000; ++id)
                if (engine.cancel(id).status == OrderStatus::CANCELLED) ++cancelled;
        });
    }
    for (auto& w : workers) w.join();

    EXPECT_EQ(cancelled.load(), 5000);   // each id cancelled exactly once, across 4 racers
    EXPECT_EQ(engine.stats().restingOrders, 0u);
    EXPECT_EQ(engine.stats().bidLevels, 0u);
}

// ===========================================================================
// REST API (SR-I-01, SR-I-02, SR-I-03) — handler level, no sockets
// ===========================================================================

class ApiHandlers : public ::testing::Test {
protected:
    MatchingEngine engine{1024};
    ApiServer api{engine};

    static json parse(const HttpResponse& r) { return json::parse(r.body); }
};

TEST_F(ApiHandlers, SubmitRestingLimitOrder) {
    const HttpResponse r = api.handleSubmit(
        R"({"orderID":1001,"orderType":"LIMIT","orderSide":"BUY","price":150.50,"quantity":100})");
    EXPECT_EQ(r.status, 201);
    const json body = parse(r);
    EXPECT_EQ(body["orderID"], 1001);
    EXPECT_EQ(body["status"], "RESTING");
    EXPECT_EQ(body["remainingQuantity"], 100);
    EXPECT_EQ(body["executedQuantity"], 0);
    EXPECT_TRUE(body["trades"].empty());
}

TEST_F(ApiHandlers, PartialFillThenCancelMatchesDocumentedContract) {
    ASSERT_EQ(api.handleSubmit(R"({"orderID":1,"orderType":"LIMIT","orderSide":"SELL","price":150.5,"quantity":60})").status, 201);
    const HttpResponse r = api.handleSubmit(
        R"({"orderID":1001,"orderType":"LIMIT","orderSide":"BUY","price":150.50,"stopPrice":149.0,"quantity":100})");
    ASSERT_EQ(r.status, 201);
    json body = parse(r);
    EXPECT_EQ(body["status"], "PARTIALLY_FILLED");
    EXPECT_EQ(body["remainingQuantity"], 40);
    EXPECT_EQ(body["executedQuantity"], 60);
    ASSERT_EQ(body["trades"].size(), 1u);
    EXPECT_EQ(body["trades"][0]["sellOrderID"], 1);
    EXPECT_DOUBLE_EQ(body["trades"][0]["tradedPrice"].get<double>(), 150.5);
    EXPECT_EQ(body["trades"][0]["tradedQuantity"], 60);

    const HttpResponse c = api.handleCancel("1001");
    EXPECT_EQ(c.status, 200);
    body = parse(c);
    EXPECT_EQ(body["orderID"], 1001);
    EXPECT_EQ(body["status"], "CANCELLED");
    EXPECT_EQ(body["remainingQuantity"], 40);
}

TEST_F(ApiHandlers, FullFillReportsFilled) {
    (void)api.handleSubmit(R"({"orderID":1,"orderSide":"SELL","price":10,"quantity":5})");
    const json body = parse(api.handleSubmit(R"({"orderID":2,"orderSide":"BUY","price":10,"quantity":5})"));
    EXPECT_EQ(body["status"], "FILLED");
    EXPECT_EQ(body["remainingQuantity"], 0);
    EXPECT_EQ(body["executedQuantity"], 5);
}

TEST_F(ApiHandlers, StopLimitIsAcceptedAndHeld) {
    const HttpResponse r = api.handleSubmit(
        R"({"orderID":7,"orderType":"STOP_LIMIT","orderSide":"BUY","price":151.0,"stopPrice":150.0,"quantity":10})");
    EXPECT_EQ(r.status, 201);
    EXPECT_EQ(parse(r)["status"], "RESTING");
    EXPECT_EQ(engine.stats().stopOrders, 1u);
    EXPECT_EQ(engine.stats().bidLevels, 0u);
}

TEST_F(ApiHandlers, TriggeredStopsAreReportedInTheTriggeringResponse) {
    (void)api.handleSubmit(R"({"orderID":1,"orderSide":"SELL","price":101,"quantity":10})");
    (void)api.handleSubmit(R"({"orderID":2,"orderType":"STOP_LIMIT","orderSide":"BUY","price":102,"stopPrice":100,"quantity":5})");
    (void)api.handleSubmit(R"({"orderID":3,"orderSide":"SELL","price":100,"quantity":1})");
    const json body = parse(api.handleSubmit(R"({"orderID":4,"orderSide":"BUY","price":100,"quantity":1})"));
    ASSERT_TRUE(body.contains("triggeredOrders"));
    ASSERT_EQ(body["triggeredOrders"].size(), 1u);
    EXPECT_EQ(body["triggeredOrders"][0]["orderID"], 2);
    EXPECT_EQ(body["triggeredOrders"][0]["status"], "FILLED");
    EXPECT_EQ(body["triggeredOrders"][0]["triggered"], true);
    EXPECT_FALSE(body.contains("triggered"));   // the aggressor was a plain limit order
}

TEST_F(ApiHandlers, CancelStopLimitOrder) {
    (void)api.handleSubmit(R"({"orderID":7,"orderType":"STOP_LIMIT","orderSide":"SELL","price":90,"stopPrice":95,"quantity":10})");
    const HttpResponse c = api.handleCancel("7");
    EXPECT_EQ(c.status, 200);
    EXPECT_EQ(parse(c)["status"], "CANCELLED");
    EXPECT_EQ(engine.stats().stopOrders, 0u);
}

TEST_F(ApiHandlers, RejectsInvalidSubmissions) {
    const char* const bad[] = {
        "",                                                                                    // empty
        "{not json",                                                                           // malformed
        "[1,2,3]",                                                                             // not an object
        R"({"orderSide":"BUY","price":1,"quantity":1})",                                       // no orderID
        R"({"orderID":-5,"orderSide":"BUY","price":1,"quantity":1})",                          // negative id
        R"({"orderID":"1","orderSide":"BUY","price":1,"quantity":1})",                         // id as string
        R"({"orderID":1,"price":1,"quantity":1})",                                             // no side
        R"({"orderID":1,"orderSide":"HOLD","price":1,"quantity":1})",                          // bad side
        R"({"orderID":1,"orderType":"MARKET","orderSide":"BUY","price":1,"quantity":1})",      // bad type
        R"({"orderID":1,"orderSide":"BUY","quantity":1})",                                     // no price
        R"({"orderID":1,"orderSide":"BUY","price":0,"quantity":1})",                           // zero price
        R"({"orderID":1,"orderSide":"BUY","price":-2.5,"quantity":1})",                        // negative price
        R"({"orderID":1,"orderSide":"BUY","price":"abc","quantity":1})",                       // non-numeric price
        R"({"orderID":1,"orderSide":"BUY","price":1})",                                        // no quantity
        R"({"orderID":1,"orderSide":"BUY","price":1,"quantity":0})",                           // zero quantity
        R"({"orderID":1,"orderSide":"BUY","price":1,"quantity":1.5})",                         // fractional quantity
        R"({"orderID":1,"orderSide":"BUY","price":1,"quantity":4294967296})",                  // > uint32
        R"({"orderID":1,"orderType":"STOP_LIMIT","orderSide":"BUY","price":1,"quantity":1})",  // stop w/o stopPrice
        R"({"orderID":1,"orderType":"STOP_LIMIT","orderSide":"BUY","price":1,"stopPrice":0,"quantity":1})",
    };
    for (const char* body : bad) {
        const HttpResponse r = api.handleSubmit(body);
        EXPECT_EQ(r.status, 400) << "body: " << body;
        EXPECT_TRUE(json::parse(r.body).contains("error")) << "body: " << body;
    }
    EXPECT_EQ(engine.stats().restingOrders, 0u);
}

TEST_F(ApiHandlers, DuplicateOrderIDIsConflict) {
    const char* const order = R"({"orderID":5,"orderSide":"BUY","price":10,"quantity":1})";
    ASSERT_EQ(api.handleSubmit(order).status, 201);
    const HttpResponse r = api.handleSubmit(order);
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(parse(r)["status"], "REJECTED");
}

TEST_F(ApiHandlers, CancelErrors) {
    EXPECT_EQ(api.handleCancel("999").status, 404);
    EXPECT_EQ(api.handleCancel("abc").status, 400);
    EXPECT_EQ(api.handleCancel("12x").status, 400);
    EXPECT_EQ(api.handleCancel("-1").status, 400);
    EXPECT_EQ(api.handleCancel("").status, 400);
    EXPECT_EQ(api.handleCancel("99999999999999999999999").status, 400);   // overflows uint64
}

// ===========================================================================
// REST API over real HTTP (loopback)
// ===========================================================================

// RAII: engine + server running on an ephemeral loopback port.
class ServerHarness {
public:
    explicit ServerHarness(std::size_t workers = 16) : api_(engine_, ApiServerConfig{.workerThreads = workers}) {
        port_ = api_.bind("127.0.0.1", 0);
        if (port_ <= 0) throw std::runtime_error("bind failed");
        thread_ = std::thread([this] { api_.run(); });
    }
    ~ServerHarness() {
        api_.stop();
        if (thread_.joinable()) thread_.join();
    }
    ServerHarness(const ServerHarness&) = delete;
    ServerHarness& operator=(const ServerHarness&) = delete;

    [[nodiscard]] int port() const { return port_; }
    MatchingEngine& engine() { return engine_; }
    ApiServer& api() { return api_; }

private:
    MatchingEngine engine_{1u << 16};
    ApiServer      api_;
    int            port_{0};
    std::thread    thread_;
};

TEST(ApiHttp, SubmitAndCancelRoundTrip) {
    ServerHarness harness;
    httplib::Client client("127.0.0.1", harness.port());

    auto post = client.Post("/api/v1/orders",
        R"({"orderID":1001,"orderType":"LIMIT","orderSide":"BUY","price":150.50,"quantity":100})",
        "application/json");
    ASSERT_TRUE(post);
    EXPECT_EQ(post->status, 201);
    EXPECT_EQ(post->get_header_value("Content-Type"), "application/json");
    const json created = json::parse(post->body);
    EXPECT_EQ(created["status"], "RESTING");
    EXPECT_EQ(created["remainingQuantity"], 100);

    auto del = client.Delete("/api/v1/orders/1001");
    ASSERT_TRUE(del);
    EXPECT_EQ(del->status, 200);
    const json cancelled = json::parse(del->body);
    EXPECT_EQ(cancelled["status"], "CANCELLED");
    EXPECT_EQ(cancelled["remainingQuantity"], 100);

    auto again = client.Delete("/api/v1/orders/1001");
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 404);
}

TEST(ApiHttp, ErrorsAreJson) {
    ServerHarness harness;
    httplib::Client client("127.0.0.1", harness.port());

    auto bad = client.Post("/api/v1/orders", "{oops", "application/json");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    EXPECT_TRUE(json::parse(bad->body).contains("error"));

    auto badID = client.Delete("/api/v1/orders/not-a-number");
    ASSERT_TRUE(badID);
    EXPECT_EQ(badID->status, 400);

    auto missing = client.Get("/api/v1/nope");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    EXPECT_TRUE(json::parse(missing->body).contains("error"));
}

TEST(ApiHttp, MatchesAcrossSeparateConnections) {
    ServerHarness harness;
    httplib::Client seller("127.0.0.1", harness.port());
    httplib::Client buyer("127.0.0.1", harness.port());

    ASSERT_TRUE(seller.Post("/api/v1/orders",
        R"({"orderID":1,"orderSide":"SELL","price":100,"quantity":60})", "application/json"));
    auto r = buyer.Post("/api/v1/orders",
        R"({"orderID":2,"orderSide":"BUY","price":100,"quantity":100})", "application/json");
    ASSERT_TRUE(r);
    const json body = json::parse(r->body);
    EXPECT_EQ(body["status"], "PARTIALLY_FILLED");
    EXPECT_EQ(body["executedQuantity"], 60);
    EXPECT_EQ(body["remainingQuantity"], 40);
}

// ===========================================================================
// Benchmarks (SR-N-01: >= 100,000 ops/s, SR-N-02: tail latency)
//
// Throughput floors are enforced only in optimised builds (NDEBUG); unoptimised
// builds just report the numbers.
// ===========================================================================

struct LatencySummary {
    double throughput{};   // ops/s
    double p50_us{}, p99_us{}, p999_us{}, max_us{};
    std::size_t count{};
};

LatencySummary summarise(std::vector<std::uint32_t>& nanos, double wallSeconds) {
    LatencySummary s;
    s.count = nanos.size();
    s.throughput = static_cast<double>(nanos.size()) / wallSeconds;
    std::ranges::sort(nanos);
    auto at = [&](double q) { return static_cast<double>(nanos[static_cast<std::size_t>(q * static_cast<double>(nanos.size() - 1))]) / 1e3; };
    s.p50_us = at(0.50); s.p99_us = at(0.99); s.p999_us = at(0.999);
    s.max_us = static_cast<double>(nanos.back()) / 1e3;
    return s;
}

void report(const char* name, const LatencySummary& s) {
    std::cout << "[ BENCH    ] " << std::left << std::setw(34) << name << std::right << std::fixed
              << std::setprecision(0) << std::setw(10) << s.throughput << " ops/s   latency us  p50="
              << std::setprecision(1) << s.p50_us << "  p99=" << s.p99_us << "  p99.9=" << s.p999_us
              << "  max=" << s.max_us << "  (n=" << s.count << ")\n";
}

// Runs `perThread` operations on `threads` threads, timing each op. `op(t, i)`.
template <typename Op>
LatencySummary runLoad(int threads, int perThread, Op op) {
    std::vector<std::vector<std::uint32_t>> samples(static_cast<std::size_t>(threads));
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        samples[static_cast<std::size_t>(t)].reserve(static_cast<std::size_t>(perThread));
        workers.emplace_back([&, t] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto& mine = samples[static_cast<std::size_t>(t)];
            for (int i = 0; i < perThread; ++i) {
                const auto start = Clock::now();
                op(t, i);
                mine.push_back(static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()));
            }
        });
    }
    while (ready.load() < threads) std::this_thread::yield();
    const auto wallStart = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    const double wall = std::chrono::duration<double>(Clock::now() - wallStart).count();

    std::vector<std::uint32_t> all;
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    return summarise(all, wall);
}

// Order flow: prices straddle a tight band so ~half the orders cross, keeping
// the book shallow but busy. Every ~5th operation cancels one of the thread's
// own earlier orders.
struct Flow {
    std::mt19937_64 rng;
    std::uint64_t   base;
    explicit Flow(int thread) : rng(99 + static_cast<unsigned>(thread)), base((static_cast<std::uint64_t>(thread) + 1) * 100'000'000ULL) {}
    std::uint64_t id(int i) const { return base + static_cast<std::uint64_t>(i); }
    double price() { return 100.0 + static_cast<double>(rng() % 21) / 100.0; }
    bool isBuy() { return rng() & 1; }
    std::uint32_t qty() { return static_cast<std::uint32_t>(1 + rng() % 100); }
};

TEST(BenchmarkEngine, SingleThreadedCore) {
    OrderBook book(1u << 20);
    Flow flow(0);
    constexpr int kOps = 1'000'000;
    const LatencySummary s = runLoad(1, kOps, [&](int, int i) {
        if (i % 5 == 4) { (void)book.cancelOrder(flow.id(i - 1)); return; }
        const bool b = flow.isBuy();
        (void)book.addOrder(b ? buy(flow.id(i), flow.price(), flow.qty()) : sell(flow.id(i), flow.price(), flow.qty()));
    });
    report("OrderBook (1 thread)", s);
#ifdef NDEBUG
    EXPECT_GT(s.throughput, 100'000.0);
    EXPECT_LT(s.p99_us, 1000.0);
#endif
}

TEST(BenchmarkEngine, ConcurrentFacade) {
    MatchingEngine engine;
    constexpr int kThreads = 8, kOps = 100'000;
    std::vector<Flow> flows;
    for (int t = 0; t < kThreads; ++t) flows.emplace_back(t);

    const LatencySummary s = runLoad(kThreads, kOps, [&](int t, int i) {
        Flow& flow = flows[static_cast<std::size_t>(t)];
        if (i % 5 == 4) { (void)engine.cancel(flow.id(i - 1)); return; }
        const bool b = flow.isBuy();
        (void)engine.submit(b ? buy(flow.id(i), flow.price(), flow.qty()) : sell(flow.id(i), flow.price(), flow.qty()));
    });
    report("MatchingEngine (8 threads)", s);
#ifdef NDEBUG
    EXPECT_GT(s.throughput, 100'000.0);
    EXPECT_LT(s.p99_us, 5000.0);
#endif
}

std::string orderJson(std::uint64_t id, bool isBuy, double price, std::uint32_t qty) {
    char buffer[200];
    std::snprintf(buffer, sizeof buffer,
        R"({"orderID":%llu,"orderType":"LIMIT","orderSide":"%s","price":%.2f,"quantity":%u})",
        static_cast<unsigned long long>(id), isBuy ? "BUY" : "SELL", price, qty);
    return buffer;
}

TEST(BenchmarkApi, HandlersWithJsonUnderConcurrency) {
    MatchingEngine engine;
    ApiServer api(engine);
    constexpr int kThreads = 8, kOps = 50'000;
    std::vector<Flow> flows;
    for (int t = 0; t < kThreads; ++t) flows.emplace_back(t);
    std::atomic<int> failures{0};

    const LatencySummary s = runLoad(kThreads, kOps, [&](int t, int i) {
        Flow& flow = flows[static_cast<std::size_t>(t)];
        HttpResponse r;
        if (i % 5 == 4) {
            r = api.handleCancel(std::to_string(flow.id(i - 1)));
            if (r.status != 200 && r.status != 404) ++failures;
        } else {
            const bool b = flow.isBuy();
            r = api.handleSubmit(orderJson(flow.id(i), b, flow.price(), flow.qty()));
            if (r.status != 201) ++failures;
        }
    });
    report("REST handlers + JSON (8 threads)", s);
    EXPECT_EQ(failures.load(), 0);
#ifdef NDEBUG
    EXPECT_GT(s.throughput, 100'000.0);
#endif
}

// Overridable for experiments: OMS_BENCH_CLIENTS=32 OMS_BENCH_OPS=20000 ./test_matching_engine
int envInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value ? std::max(1, std::atoi(value)) : fallback;
}

TEST(BenchmarkApi, HttpLoopbackUnderConcurrency) {
    const int kClients = envInt("OMS_BENCH_CLIENTS", 8);
    const int kOps = envInt("OMS_BENCH_OPS", 5'000);
    ServerHarness harness(static_cast<std::size_t>(kClients) * 2);
    std::vector<Flow> flows;
    std::vector<std::unique_ptr<httplib::Client>> clients;
    for (int t = 0; t < kClients; ++t) {
        flows.emplace_back(t);
        clients.push_back(std::make_unique<httplib::Client>("127.0.0.1", harness.port()));
        clients.back()->set_keep_alive(true);
        clients.back()->set_tcp_nodelay(true);
    }
    std::atomic<int> failures{0};

    const LatencySummary s = runLoad(kClients, kOps, [&](int t, int i) {
        Flow& flow = flows[static_cast<std::size_t>(t)];
        httplib::Client& client = *clients[static_cast<std::size_t>(t)];
        if (i % 5 == 4) {
            auto r = client.Delete("/api/v1/orders/" + std::to_string(flow.id(i - 1)));
            if (!r || (r->status != 200 && r->status != 404)) ++failures;
        } else {
            const bool b = flow.isBuy();
            auto r = client.Post("/api/v1/orders", orderJson(flow.id(i), b, flow.price(), flow.qty()), "application/json");
            if (!r || r->status != 201) ++failures;
        }
    });
    const std::string label = "HTTP loopback (" + std::to_string(kClients) + " clients)";
    report(label.c_str(), s);
    EXPECT_EQ(failures.load(), 0);

    // Server-side state must agree with what the clients did.
    const BookStats stats = harness.engine().stats();
    EXPECT_TRUE(stats.bidQuantity == 0 || stats.askQuantity == 0 || *stats.bestBid < *stats.bestAsk);
#ifdef NDEBUG
    EXPECT_LT(s.p99_us, 50'000.0);
#endif
}

// ---------------------------------------------------------------------------
// SR-N-01 / SR-N-02 acceptance test
//
//   SR-N-01  >= 100,000 operations/s sustained under continuous load, measured
//            end-to-end over HTTP (client -> socket -> parse -> match ->
//            serialise -> socket -> client), in every full one-second window.
//   SR-N-02  p99 order-to-fill round-trip latency <= 1 ms under that same load.
//            "Order-to-fill" = a submit whose response reports executed
//            quantity > 0, timed from before the request is written until the
//            full response has been read.
//
// The SRS targets Linux, so the floors are enforced on Linux optimised builds
// with >= 4 hardware threads (client and server share the machine). Elsewhere
// (macOS loopback is several times slower, sanitizers distort timing) the
// numbers are reported but not asserted. OMS_BENCH_ENFORCE=0|1 overrides.
// ---------------------------------------------------------------------------

constexpr double kSrN01MinOpsPerSecond = 100'000.0;
constexpr double kSrN02P99Microseconds = 1'000.0;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
[[maybe_unused]] constexpr bool kInstrumentedBuild = true;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
[[maybe_unused]] constexpr bool kInstrumentedBuild = true;
#  else
[[maybe_unused]] constexpr bool kInstrumentedBuild = false;
#  endif
#else
[[maybe_unused]] constexpr bool kInstrumentedBuild = false;
#endif

bool slaEnforced() {
    if (const char* v = std::getenv("OMS_BENCH_ENFORCE")) return std::atoi(v) != 0;
#if defined(NDEBUG) && defined(__linux__)
    return !kInstrumentedBuild && std::thread::hardware_concurrency() >= 4;
#else
    return false;
#endif
}

TEST(BenchmarkApi, SustainedHttpMeetsThroughputAndLatencySla) {
    const int kClients = envInt("OMS_BENCH_CLIENTS", 8);
    const int kSeconds = envInt("OMS_BENCH_SECONDS", 3);
    constexpr auto kWarmup = std::chrono::milliseconds(500);

    ServerHarness harness(static_cast<std::size_t>(kClients) * 2);
    struct ClientState {
        std::unique_ptr<httplib::Client> http;
        Flow flow;
        std::vector<std::uint32_t> windowOps;     // completed ops per measured second
        std::vector<std::uint32_t> allNanos;      // every op
        std::vector<std::uint32_t> fillNanos;     // submits that produced fills
        std::uint64_t failures{0};
        std::string firstFailure;                 // diagnostic for the first failed request
        explicit ClientState(int t, int seconds) : flow(t), windowOps(static_cast<std::size_t>(seconds), 0) {}
    };
    std::vector<std::unique_ptr<ClientState>> clients;
    for (int t = 0; t < kClients; ++t) {
        auto c = std::make_unique<ClientState>(t, kSeconds);
        c->http = std::make_unique<httplib::Client>("127.0.0.1", harness.port());
        c->http->set_keep_alive(true);
        c->http->set_tcp_nodelay(true);
        c->allNanos.reserve(1u << 20);
        c->fillNanos.reserve(1u << 19);
        clients.push_back(std::move(c));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<Clock::rep> startTicks{0};   // when measurement begins (after warm-up)
    std::vector<std::thread> workers;
    for (int t = 0; t < kClients; ++t) {
        workers.emplace_back([&, t] {
            ClientState& c = *clients[static_cast<std::size_t>(t)];
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto measureStart = Clock::time_point(Clock::duration(startTicks.load()));
            const auto end = measureStart + std::chrono::seconds(kSeconds);
            for (int i = 0; ; ++i) {
                const auto t0 = Clock::now();
                if (t0 >= end) break;
                bool isFillCandidate = false;
                bool ok;
                if (i % 5 == 4) {
                    auto r = c.http->Delete("/api/v1/orders/" + std::to_string(c.flow.id(i - 1)));
                    ok = r && (r->status == 200 || r->status == 404);
                    if (!ok && c.firstFailure.empty())
                        c.firstFailure = "op#" + std::to_string(i) + " t+" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - measureStart).count()) + "ms DELETE: " + (r ? "HTTP " + std::to_string(r->status) : httplib::to_string(r.error()));
                } else {
                    const bool b = c.flow.isBuy();
                    auto r = c.http->Post("/api/v1/orders", orderJson(c.flow.id(i), b, c.flow.price(), c.flow.qty()),
                                          "application/json");
                    ok = r && r->status == 201;
                    if (!ok && c.firstFailure.empty())
                        c.firstFailure = "op#" + std::to_string(i) + " t+" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - measureStart).count()) + "ms POST: " + (r ? "HTTP " + std::to_string(r->status) + " " + r->body : httplib::to_string(r.error()));
                    // Compact nlohmann output with sorted keys: executedQuantity is never the last key.
                    isFillCandidate = ok && r->body.find("\"executedQuantity\":0,") == std::string::npos;
                }
                const auto t1 = Clock::now();
                if (!ok) { ++c.failures; continue; }
                if (t0 < measureStart) continue;   // warm-up
                const auto nanos = static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                const auto window = static_cast<std::size_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(t1 - measureStart).count());
                if (window < c.windowOps.size()) ++c.windowOps[window];
                c.allNanos.push_back(nanos);
                if (isFillCandidate) c.fillNanos.push_back(nanos);
            }
        });
    }
    while (ready.load() < kClients) std::this_thread::yield();
    // Warm-up runs inside the same loop: clients start now, measurement starts kWarmup later.
    startTicks.store((Clock::now() + kWarmup).time_since_epoch().count());
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    std::vector<double> perSecond(static_cast<std::size_t>(kSeconds), 0.0);
    std::vector<std::uint32_t> all, fills;
    std::uint64_t failures = 0;
    for (auto& c : clients) {
        for (std::size_t w = 0; w < perSecond.size(); ++w) perSecond[w] += c->windowOps[w];
        all.insert(all.end(), c->allNanos.begin(), c->allNanos.end());
        fills.insert(fills.end(), c->fillNanos.begin(), c->fillNanos.end());
        failures += c->failures;
        if (!c->firstFailure.empty()) std::cout << "[ BENCH    ] request failure: " << c->firstFailure << "\n";
    }
    ASSERT_FALSE(all.empty());
    ASSERT_FALSE(fills.empty()) << "workload produced no fills; order-to-fill latency is unmeasurable";

    const double minWindow = *std::ranges::min_element(perSecond);
    const LatencySummary overall = summarise(all, static_cast<double>(kSeconds));
    const LatencySummary fill = summarise(fills, static_cast<double>(kSeconds));
    report(("HTTP sustained " + std::to_string(kSeconds) + "s, all ops").c_str(), overall);
    report("HTTP sustained, order-to-fill", fill);
    std::cout << "[ BENCH    ] SR-N-01 min 1s window = " << std::fixed << std::setprecision(0) << minWindow
              << " ops/s (floor " << kSrN01MinOpsPerSecond << ");  SR-N-02 order-to-fill p99 = "
              << std::setprecision(1) << fill.p99_us << " us (ceiling " << kSrN02P99Microseconds << ")  ["
              << (slaEnforced() ? "enforced" : "report-only") << "]\n";

    EXPECT_EQ(failures, 0u);
    if (slaEnforced()) {
        EXPECT_GE(minWindow, kSrN01MinOpsPerSecond) << "SR-N-01: slowest one-second window fell below 100k ops/s";
        EXPECT_GE(overall.throughput, kSrN01MinOpsPerSecond);
        EXPECT_LE(fill.p99_us, kSrN02P99Microseconds) << "SR-N-02: order-to-fill p99 exceeded 1 ms";
    }
}

}  // namespace
