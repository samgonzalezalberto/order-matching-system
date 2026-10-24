# Requirements Traceability Matrix

Companion to `system-requirements-specification.md` (IEEE 29148 style).
Each requirement maps to the implementation that satisfies it and to the tests that verify it.
Test names are `Suite.Test` as reported by GoogleTest (`ctest -N` lists them all).

Verification methods: **T** = automated test, **I** = inspection (design or code review).

| ID | Requirement (short) | Implementation | Verification |
|----|---------------------|----------------|--------------|
| SR-F-01 | Accept Limit orders with unique `orderID`, side, price, quantity | `Order::limit`, `OrderBook::addOrder` (validation, duplicate check) in `include/order_book.hpp`; `ApiServer::handleSubmit` | T: `OrderBookValidation.*`, `ApiHandlers.SubmitRestingLimitOrder`, `ApiHandlers.RejectsInvalidSubmissions`, `ApiHandlers.DuplicateOrderIDIsConflict` (see DN-1) |
| SR-F-02 | Execute crossing Limit orders immediately | `OrderBook::executeLimit` | T: `OrderBookPriceTime.*`, `OrderBookPartial.SweepsMultipleLevels` |
| SR-F-03 | Accept Stop-Limit orders with `stopPrice` | `Order::stopLimit`, `OrderBook::parkStopOrder` | T: `StopLimit.RegistersInSecondaryQueueNotInTheBook`, `ApiHandlers.StopLimitIsAcceptedAndHeld` |
| SR-F-04 | Hold stops in a secondary queue; activate when last trade reaches/crosses `stopPrice` | `OrderBook::evaluateStopOrders` (iterative, handles cascades) | T: `StopLimit.BuyStopTriggersWhenLastPriceReachesStopPrice`, `...SellStop...`, `...Dormant...`, `MultiStage*Cascade`, `CascadeCanCrossSides`, `SimultaneousTriggersActivateInTimePriority`, `ActivatesImmediatelyIfMarketAlreadyThroughStopPrice`, `ApiHandlers.TriggeredStopsAreReportedInTheTriggeringResponse` |
| SR-F-05 | Strict Price-Time priority | `std::map` price ordering + FIFO `std::list` tiers; `Order::sequence` for simultaneous triggers | T: `OrderBookPriceTime.*` |
| SR-F-06 | Partial fills reduce both orders by `min(qty)` | `OrderBook::executeLimit` | T: `OrderBookPartial.*` |
| SR-F-07 | Residual rests at the back of its tier | `OrderBook::executeLimit` / tier append | T: `OrderBookPartial.ResidualJoinsBackOfTier`, `IncomingPartialFillRestsResidual` |
| SR-F-08 | Cancel by `orderID` removes order from book | `OrderBook::cancelOrder`, `ApiServer::handleCancel` | T: `OrderBookCancel.*`, `StopLimit.CancelStopOrderKeepsIndexConsistent`, `ApiHandlers.CancelErrors` |
| SR-F-09 | O(1) average cancellation via `orderIndex` | `OrderLocation` (stored list and tier iterators) | T: `OrderBookCancel.CancellationCostIsIndependentOfBookSize`, `NoTierLeakAfterHeavyAddCancelChurn`; I |
| SR-I-01 | `POST /api/v1/orders`, JSON | `ApiServer` route + `handleSubmit` | T: `ApiHandlers.*`, `ApiHttp.SubmitAndCancelRoundTrip`, `ApiHttp.ErrorsAreJson` |
| SR-I-02 | `DELETE /api/v1/orders/{orderID}` | `ApiServer` route + `handleCancel` | T: `ApiHandlers.CancelErrors`, `ApiHttp.SubmitAndCancelRoundTrip` |
| SR-I-03 | Synchronous 200/201 with status in {`FILLED`,`PARTIALLY_FILLED`,`RESTING`,`CANCELLED`} and remaining quantity | `reportToJson` in `src/api_server.cpp`; `OrderStatus` in `include/order.hpp` | T: `ApiHandlers.PartialFillThenCancelMatchesDocumentedContract`, `FullFillReportsFilled`, `StopLimit.ActivatedStopThatOnlyRestsReportsRestingWithTriggeredFlag` (see DN-3) |
| SR-N-01 | >= 100,000 ops/s sustained under continuous load | Mutex-guarded `MatchingEngine`; I/O and JSON on caller threads | T: `BenchmarkApi.SustainedHttpMeetsThroughputAndLatencySla` (every 1 s window, end-to-end over HTTP); supporting: `BenchmarkEngine.*`, `BenchmarkApi.Handlers*`, `MatchingEngineConcurrency.*` (see DN-2) |
| SR-N-02 | p99 order-to-fill round-trip latency SLA under peak load | same | T: same test, **p99 <= 1 ms** for submits that produced a fill (see DN-2) |
| SR-N-03 | Assume risk checks done upstream | Engine performs only structural validation (positive finite price/quantity, ID uniqueness) | I |
| SR-D-01 | `std::map` tiers holding FIFO `std::list<Order>` | `BidLevels`, `AskLevels` in `include/order_book.hpp` | I; behaviour covered by `OrderBookPriceTime.*` |
| SR-D-02 | `std::unordered_map<uint64_t, OrderLocation>` for every active order | `OrderBook::orderIndex` | I; `OrderBookCancel.*`, `StopLimit.RandomisedWorkloadPreservesInvariants` |
| SR-D-03 | RAM only; no WAL or snapshots | No file or disk I/O anywhere in the engine | I |

## Design notes (resolutions of SRS ambiguities)

**DN-1: `orderID` uniqueness scope (SR-F-01).**
Uniqueness is enforced among *active* orders, which is the scope SR-D-02 indexes. An ID may be reused once its order has been fully filled or cancelled, and a rejected order never reserves its ID.
Lifetime uniqueness would require remembering every ID ever seen, which grows without bound in a non-persistent engine (SR-D-03). Cross-time uniqueness is therefore the upstream gateway's responsibility (SR-N-03).
Tests: `OrderBookValidation.RejectsDuplicateActiveOrderID`, `AllowsIDReuseAfterOrderLeavesTheBook`, `FullyFilledOrderIDCanBeReused`, `RejectedOrderDoesNotReserveItsID`, `StopLimit.DuplicateIDAcrossStopAndLimitIsRejected`.

**DN-2: Quantified performance targets (SR-N-01, SR-N-02).**
SR-N-02 gave no number. The agreed target is **p99 <= 1 ms** order-to-fill round trip, measured at the HTTP boundary (client write start to full response read) with 8 concurrent keep-alive clients, alongside the SR-N-01 floor of **100,000 ops/s in every full one-second window** (3 s measured after a 0.5 s warm-up, 80% POST / 20% DELETE flow, about 1/3 of operations producing fills).
Constants: `kSrN01MinOpsPerSecond`, `kSrN02P99Microseconds` in `tests/test_matching_engine.cpp`.
Enforcement: the SRS targets Linux, so assertions run on Linux optimised builds with >= 4 hardware threads, not under sanitizers. Elsewhere the numbers are reported only (macOS loopback is several times slower). `OMS_BENCH_ENFORCE=0|1` overrides.
Measured on Linux (10 vCPU, client and server on the same host): about 125k-150k ops/s minimum per 1 s window, order-to-fill p99 about 270-330 us at 8 clients; about 230k ops/s and p99 about 430 us at 16 clients.

**DN-3: Status values (SR-I-03).**
Successful (200/201) responses only ever carry one of the four SRS statuses. Stop-Limit activation is reported by a separate boolean `"triggered": true` (and by the entry appearing in `triggeredOrders`), so a stop that activates and merely rests reports `RESTING`. `REJECTED` appears only in error (4xx) bodies, which SR-I-03 does not cover.

## Known limitations

* **Prices are `double`** as mandated by SR-F-01. Equality and ordering use exact binary comparison, so two prices that print identically but were computed differently may not share a tier. The upstream gateway should send prices already rounded to the instrument's tick. A fixed-point tick representation would require an SRS change.
* **Stress below the SLA configuration:** with 16 client threads on 10 vCPUs (more runnable client and server threads than cores), about 1 run in 5 saw a single dropped keep-alive connection among ~0.8M requests. No server-side error is logged and it never occurred in 20 runs at 8 clients. Root cause was not identified; production gateways should retry idempotently on transport errors.
