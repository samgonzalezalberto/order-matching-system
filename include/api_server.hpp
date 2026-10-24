#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "matching_engine.hpp"

struct ApiServerConfig {
    // Worker threads serving HTTP connections. cpp-httplib dedicates one worker
    // to each open keep-alive connection, so size this >= expected concurrent
    // client connections.
    std::size_t workerThreads{16};
};

struct HttpResponse {
    int         status{200};
    std::string body;
};

// REST front-end for the matching engine (SR-I-01, SR-I-02, SR-I-03).
//
//   POST   /api/v1/orders              submit a LIMIT or STOP_LIMIT order
//   DELETE /api/v1/orders/{orderID}    cancel an order
//
// The request handlers are transport independent (string in, string out) so
// they can be exercised without opening sockets.
class ApiServer {
public:
    explicit ApiServer(MatchingEngine& engine, ApiServerConfig config = {});
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Binds and starts listening (connections queue in the backlog from this
    // point on). `port == 0` selects any free port. Returns the bound port, or
    // -1 on failure.
    [[nodiscard]] int bind(const std::string& host, int port);

    // Serves requests; blocks until stop() is called. Call after bind().
    bool run();

    // Thread-safe; makes run() return.
    void stop();

    // POST /api/v1/orders
    [[nodiscard]] HttpResponse handleSubmit(std::string_view body);
    // DELETE /api/v1/orders/{orderID}
    [[nodiscard]] HttpResponse handleCancel(std::string_view orderIDText);

private:
    struct Impl;   // keeps <httplib.h> out of every includer
    MatchingEngine&       engine_;
    std::unique_ptr<Impl> impl_;
};
