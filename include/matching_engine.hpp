#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

#include "order.hpp"
#include "order_book.hpp"

// Thread-safe façade over OrderBook (SR-N-01 / SR-N-02).
//
// Concurrency model: a single mutex serialises mutations of the book. Matching
// is a short, allocation-light, in-memory operation (well under a microsecond
// for typical orders), so the critical section is tiny; everything expensive
// (socket I/O, JSON parsing/serialisation) happens on the caller's thread
// *outside* the lock. Serialising every operation through one lock is also
// what guarantees deterministic price-time priority: the lock acquisition
// order *is* the arrival order.
class MatchingEngine {
public:
    explicit MatchingEngine(std::size_t expectedOrders = 1u << 20) : book_(expectedOrders) {}

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    ExecutionReport submit(Order order) {
        std::lock_guard<std::mutex> guard(mutex_);
        return book_.addOrder(std::move(order));
    }

    ExecutionReport cancel(std::uint64_t orderID) {
        std::lock_guard<std::mutex> guard(mutex_);
        return book_.cancelOrder(orderID);
    }

    // Consistent snapshot of the book (O(n); diagnostics and tests only).
    [[nodiscard]] BookStats stats() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return book_.stats();
    }

    [[nodiscard]] bool contains(std::uint64_t orderID) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return book_.contains(orderID);
    }

private:
    mutable std::mutex mutex_;
    OrderBook book_;
};
