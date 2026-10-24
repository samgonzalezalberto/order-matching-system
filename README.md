# Order Matching System

A high-throughput, low-latency, in-memory order matching engine written in C++20, exposed over a small HTTP REST API.

The project was built to conform to a written specification, `docs/system-requirements-specification.md` (IEEE 29148 style). Every requirement is traced to code and tests in `docs/requirements-traceability.md`.

## Features

- **Limit orders** with strict **price-time priority**, partial fills, and residual quantity resting at the back of its price tier.
- **Stop-Limit orders**, held in a secondary queue and activated when the last traded price reaches or crosses `stopPrice`. Multi-stage cascades are handled iteratively, without recursion.
- **O(1) cancellation** through an index of iterators into the book.
- **REST API** with synchronous execution reports.
- **Volatile state only.** There is no disk I/O, WAL or snapshotting (SR-D-03).

Design in brief: the book is an ordered `std::map` of price tiers, each a FIFO `std::list<Order>`. A hash map from `orderID` to the order's location in the book makes cancels constant-time. A mutex-guarded `MatchingEngine` serializes book mutations, while socket I/O and JSON work run on the HTTP worker threads, outside the lock.

## Repository layout

```
include/order.hpp            order, trade, status and report types
include/order_book.hpp       limit order book, matching, stop-limit queue
include/matching_engine.hpp  thread-safe facade over the order book
include/api_server.hpp       REST layer interface
src/api_server.cpp           HTTP routes, JSON parsing and serialization
src/main.cpp                 server executable (CLI flags, clean shutdown)
tests/test_matching_engine.cpp   unit, integration, concurrency and benchmark tests
docs/                        SRS and requirements traceability matrix
Dockerfile, docker-compose.yml
```

## Build and run

Requirements: a C++20 compiler (GCC or Clang), CMake 3.20+, and network access on the first configure. CMake fetches `cpp-httplib`, `nlohmann/json` and GoogleTest.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/order_matching_engine --port 8080
```

Server options:

| Flag | Default | Meaning |
|------|---------|---------|
| `--host` | `0.0.0.0` | bind address |
| `--port` | `8080` | TCP port (`0` picks a free port) |
| `--threads` | `16` | HTTP worker threads (one per concurrent connection) |

Stop it with `Ctrl-C` (SIGINT) or SIGTERM. It shuts down cleanly.

CMake options: `-DOMS_NATIVE_ARCH=OFF` for a portable binary (the default tunes for the build machine), `-DOMS_BUILD_TESTS=OFF` to skip the tests.

### Docker

```sh
docker compose up -d --build                 # runtime image, serves on :8080
docker compose --profile test build test     # build and run the full test suite
docker build --target test .                 # same, without compose
```

The test stage fails the build if any test fails.

## API

### `POST /api/v1/orders`: submit an order

| Field | Type | Notes |
|-------|------|-------|
| `orderID` | unsigned integer | must be unique among active orders |
| `orderType` | `"LIMIT"` or `"STOP_LIMIT"` | optional, defaults to `LIMIT` |
| `orderSide` | `"BUY"` or `"SELL"` | |
| `price` | number > 0 | limit price |
| `quantity` | integer, 1 to 4294967295 | |
| `stopPrice` | number > 0 | required for `STOP_LIMIT` only |

```sh
curl -X POST localhost:8080/api/v1/orders -H 'Content-Type: application/json' \
  -d '{"orderID":1,"orderType":"LIMIT","orderSide":"SELL","price":101.50,"quantity":100}'
# 201 {"executedQuantity":0,"orderID":1,"originalQuantity":100,"price":101.5,"remainingQuantity":100,"status":"RESTING","trades":[]}

curl -X POST localhost:8080/api/v1/orders -H 'Content-Type: application/json' \
  -d '{"orderID":2,"orderSide":"BUY","price":101.50,"quantity":40}'
# 201 {"executedQuantity":40,"orderID":2,"originalQuantity":40,"price":101.5,"remainingQuantity":0,"status":"FILLED",
#      "trades":[{"buyOrderID":2,"sellOrderID":1,"tradedPrice":101.5,"tradedQuantity":40}]}
```

`status` is one of `FILLED`, `PARTIALLY_FILLED` or `RESTING`. Trades print at the resting order's price.

If the request causes Stop-Limit orders to activate, they are listed in `triggeredOrders`, each with its own execution report and `"triggered": true`. A Stop-Limit order that activates immediately on arrival carries `"triggered": true` on its own response.

### `DELETE /api/v1/orders/{orderID}`: cancel an order

```sh
curl -X DELETE localhost:8080/api/v1/orders/1
# 200 {"orderID":1,"remainingQuantity":60,"status":"CANCELLED"}
```

Works for resting limit orders and for stop orders that have not triggered yet.

### Errors

Errors are JSON with an `error` field.

| HTTP status | When |
|-------------|------|
| 400 | malformed JSON or an invalid field |
| 404 | cancel of an unknown or already-removed order, or an unknown route |
| 409 | `orderID` already active (`status: "REJECTED"`) |

An `orderID` is unique among *active* orders. Once an order is fully filled or cancelled, its ID may be reused. Rejected orders never reserve an ID.

## Tests and benchmarks

```sh
ctest --test-dir build --output-on-failure       # all 64 tests
./build/test_matching_engine --gtest_filter='Benchmark*'    # benchmarks with printed numbers
```

The suite covers price-time priority, partial fills, cancellation and tier cleanup, stop-limit triggers and cascades, concurrency invariants, the REST contract (handlers and real HTTP), and a randomized invariant test.

### Performance (SR-N-01, SR-N-02)

`BenchmarkApi.SustainedHttpMeetsThroughputAndLatencySla` runs 8 keep-alive HTTP clients for 3 seconds against the real server and checks:

- at least **100,000 ops/s in every one-second window**, and
- **p99 order-to-fill latency of 1 ms or less**, measured from request write to full response read.

On a 10-vCPU Linux container, with client and server on the same machine, I measured roughly 125k to 150k ops/s in the slowest window and a p99 of about 270 to 330 µs. The engine alone does millions of operations per second, and the HTTP layer is the limit.

These floors are enforced on Linux optimized builds with at least 4 hardware threads, which is the platform the SRS targets. On other platforms, or under sanitizers, the numbers are printed but not asserted. macOS loopback is several times slower, around 50k ops/s. Set `OMS_BENCH_ENFORCE=0` or `1` to override. `OMS_BENCH_CLIENTS` and `OMS_BENCH_SECONDS` change the load.

## Design decisions and known limitations

- **ID uniqueness** is among active orders only, matching the SRS's active-order index and keeping memory bounded. Uniqueness across time is the upstream gateway's job (SR-N-03).
- **Status values** in successful responses are only the four SRS ones. Stop activation is reported through the separate `triggered` flag.
- **Prices are `double`**, as the SRS requires. Matching uses exact comparisons, so a gateway should send prices already rounded to the instrument's tick size.
- **No pre-trade risk checks**, no authentication and no market data feed. The engine assumes sanitized input from an upstream gateway (SR-N-03).
- **State is lost on restart** by design.
- **Overload behavior:** in stress runs with more client threads than CPU cores (16 clients on 10 vCPUs), I occasionally saw a single dropped keep-alive connection among hundreds of thousands of requests. I didn't find the root cause. Clients of a real deployment should retry on transport errors.

See `docs/requirements-traceability.md` for the full requirement-to-test mapping and the reasoning behind these decisions.
