# Software Requirements Specification (SRS)

## High-Throughput Open-Market Order Matching Engine

---

### 1. Introduction

#### 1.1 Purpose

This Software Requirements Specification (SRS) defines the functional, non-functional, interface, and structural requirements for the High-Throughput Open-Market Order Matching Engine. This document serves as the formal baseline for development, testing, and validation according to the IEEE 29148 standard.

#### 1.2 Scope

The system is a deterministic, high-throughput, in-memory matching core designed to execute financial buy and sell orders. The scope encompasses:

* Ingestion and matching of Limit and Stop-Limit orders.
* Enforcement of strict Price-Time Priority (FIFO) execution logic.
* Provisioning of a RESTful HTTP API for external participant communication.
* In-memory order book state management without persistence overhead.

Out of scope: Upstream pre-trade risk management, user authentication, margin checks, post-trade settlement clearing, and disk-based Write-Ahead Logging (WAL).

#### 1.3 Acronyms and Definitions

* **API:** Application Programming Interface
* **FIFO:** First-In, First-Out (Time Priority)
* **HTTP:** Hypertext Transfer Protocol
* **LOB:** Limit Order Book
* **REST:** Representational State Transfer
* **SLA:** Service Level Agreement

---

### 2. Overall Description

#### 2.1 Product Perspective

The Order Matching Engine operates as a core execution service within a multi-tiered exchange architecture. It receives sanitized order requests from an upstream Pre-Trade Risk & Gateway layer via REST, executes matches against an in-memory Limit Order Book, and returns execution receipts.

```
+------------------------+        REST HTTP        +-------------------------------+
|  Upstream Gateway /    | ----------------------> |    Order Matching Engine      |
| Pre-Trade Risk Service | <---------------------- | (In-Memory Price-Time Core)   |
+------------------------+        Responses        +-------------------------------+

```

#### 2.2 Stakeholder Summary

* **External Market Participants (Buyers / Sellers):** Submit limit/stop-limit orders and cancellation requests via REST endpoints.
* **Exchange Operators & System Administrators:** Monitor system throughput, latency SLAs, and operational availability.
* **Systems Developers:** Maintain, profile, and extend the C++ matching core.

#### 2.3 Operating Environment

* **Language/Standard:** C++20 or higher.
* **Host Operating System:** Linux (POSIX-compliant, kernel-tuned for low-latency networking).
* **Network Infrastructure:** High-bandwidth local enterprise network or isolated cloud instance handling HTTP REST traffic.

#### 2.4 Design Constraints

* **In-Memory Volatility:** System state resides strictly in volatile RAM. No Write-Ahead Logging (WAL) or database recovery mechanism is implemented; system termination results in state reset.
* **Upstream Risk Reliance:** The engine performs zero inline account balance, credit, or fat-finger validations, delegating all safety checks upstream.

---

### 3. Specific System Requirements

#### 3.1 Functional Requirements

##### 3.1.1 Order Ingestion & Types

* **SR-F-01 (Limit Order Handling):** The system shall accept Limit Orders containing a unique `orderID` (`uint64_t`), `orderSide` (`Buy` or `Sell`), `orderPrice` (`double`), and `orderQuantity` (`uint32_t`).
* **SR-F-02 (Limit Order Execution):** The system shall immediately execute an incoming Limit Order against resting liquidity on the opposite side of the order book if `orderPrice` crosses the best available price (`bestAskPrice <= orderPrice` for Buys; `bestBidPrice >= orderPrice` for Sells).
* **SR-F-03 (Stop-Limit Order Registration):** The system shall accept Stop-Limit Orders containing a `stopPrice` trigger condition alongside standard Limit Order attributes (`orderPrice`, `orderQuantity`, `orderSide`).
* **SR-F-04 (Stop-Limit Order Activation):** The system shall hold Stop-Limit Orders in a secondary evaluation queue and shall activate a Stop-Limit Order into a standard Limit Order as soon as the last traded execution price reaches or crosses `stopPrice`.

##### 3.1.2 Matching Logic & Execution

* **SR-F-05 (Price-Time Priority):** The system shall match orders strictly according to Price-Time Priority:
1. **Price Priority:** Higher bid prices and lower ask prices take precedence.
2. **Time Priority:** Orders at the same price tier are matched in strict FIFO sequence based on insertion timestamp/order ID.


* **SR-F-06 (Partial Fill Processing):** The system shall reduce the `orderQuantity` of both incoming and resting orders upon execution by `tradedQuantity = min(incoming.quantity, resting.quantity)`.
* **SR-F-07 (Residual Allocation):** If an incoming Limit Order is partially filled, the system shall append the remaining unexecuted `orderQuantity` to the back of the corresponding price tier in the order book.

##### 3.1.3 Order Cancellation

* **SR-F-08 (Cancellation Execution):** The system shall process cancellation requests containing an `orderID` and remove the target order from the active order book.
* **SR-F-09 (Constant-Time Removal):** The system shall execute order cancellations in $O(1)$ average time complexity using direct memory iterator lookups via `orderIndex`.

---

#### 3.2 External Interface Requirements

##### 3.2.1 REST API Interface

* **SR-I-01 (Order Submission Endpoint):** The system shall expose an HTTP REST endpoint (`POST /api/v1/orders`) accepting JSON payloads to submit Limit and Stop-Limit orders.
* **SR-I-02 (Order Cancellation Endpoint):** The system shall expose an HTTP REST endpoint (`DELETE /api/v1/orders/{orderID}`) accepting cancellation requests.
* **SR-I-03 (Execution Response Format):** The system shall return synchronous HTTP 200/201 responses containing order status (`FILLED`, `PARTIALLY_FILLED`, `RESTING`, `CANCELLED`) and remaining quantity.

---

#### 3.3 Non-Functional Requirements

##### 3.3.1 Performance & Latency SLAs

* **SR-N-01 (Throughput Capacity):** The system shall sustain a minimum processing throughput of **100,000 operations per second** (orders/sec) under continuous load.
* **SR-N-02 (Latency SLA):** The system shall achieve a **$99\text{th}$ percentile order-to-fill round-trip latency** SLA under peak operational load.

##### 3.3.2 Availability & Resilience

* **SR-N-03 (Upstream Risk Delegation):** The matching engine shall process incoming order payloads assuming all pre-trade risk validations have been completed upstream.

---

#### 3.4 Data & State Requirements

##### 3.4.1 In-Memory State Architecture

* **SR-D-01 (State Representation):** The system shall maintain bid and ask price tiers using ordered balanced trees (`std::map`), where price tiers store sequential FIFO queues (`std::list<Order>`).
* **SR-D-02 (Direct Index Lookup):** The system shall maintain an in-memory hash table (`std::unordered_map<uint64_t, OrderLocation>`) mapping every active `orderID` to its price level, side, and iterator location within the order list.
* **SR-D-03 (Non-Persistent Operation):** The system shall store all book states exclusively in volatile memory (RAM) and shall operate without Write-Ahead Logging (WAL) or disk snapshotting mechanisms.