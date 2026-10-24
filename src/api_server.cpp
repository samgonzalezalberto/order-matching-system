#include "api_server.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

constexpr const char* kJson = "application/json";

HttpResponse errorResponse(int status, std::string_view message) {
    json body;
    body["error"] = std::string(message);
    return {status, body.dump()};
}

// ---- request parsing --------------------------------------------------------

struct ParseResult {
    std::optional<Order> order;
    std::string          error;
};

ParseResult parseOrder(const json& doc) {
    auto fail = [](std::string message) { return ParseResult{std::nullopt, std::move(message)}; };

    if (!doc.is_object()) return fail("request body must be a JSON object");

    // orderID
    const auto idIt = doc.find("orderID");
    if (idIt == doc.end() || !idIt->is_number_unsigned())
        return fail("'orderID' is required and must be an unsigned integer");
    const auto orderID = idIt->get<std::uint64_t>();

    // orderType (defaults to LIMIT)
    OrderType type = OrderType::Limit;
    if (const auto it = doc.find("orderType"); it != doc.end()) {
        if (!it->is_string()) return fail("'orderType' must be a string");
        const auto& text = it->get_ref<const std::string&>();
        if (text == "LIMIT")           type = OrderType::Limit;
        else if (text == "STOP_LIMIT") type = OrderType::StopLimit;
        else return fail("'orderType' must be LIMIT or STOP_LIMIT");
    }

    // orderSide
    const auto sideIt = doc.find("orderSide");
    if (sideIt == doc.end() || !sideIt->is_string())
        return fail("'orderSide' is required and must be BUY or SELL");
    Side side;
    if (const auto& text = sideIt->get_ref<const std::string&>(); text == "BUY") side = Side::Buy;
    else if (text == "SELL") side = Side::Sell;
    else return fail("'orderSide' must be BUY or SELL");

    // price
    const auto priceIt = doc.find("price");
    if (priceIt == doc.end() || !priceIt->is_number())
        return fail("'price' is required and must be a number");
    const double price = priceIt->get<double>();
    if (!std::isfinite(price) || price <= 0.0) return fail("'price' must be a positive number");

    // quantity
    const auto qtyIt = doc.find("quantity");
    if (qtyIt == doc.end() || !qtyIt->is_number_unsigned())
        return fail("'quantity' is required and must be an unsigned integer");
    const auto quantity = qtyIt->get<std::uint64_t>();
    if (quantity == 0 || quantity > std::numeric_limits<std::uint32_t>::max())
        return fail("'quantity' must be between 1 and 4294967295");

    // stopPrice (required for STOP_LIMIT, ignored for LIMIT)
    double stopPrice = 0.0;
    if (type == OrderType::StopLimit) {
        const auto stopIt = doc.find("stopPrice");
        if (stopIt == doc.end() || !stopIt->is_number())
            return fail("'stopPrice' is required for STOP_LIMIT orders");
        stopPrice = stopIt->get<double>();
        if (!std::isfinite(stopPrice) || stopPrice <= 0.0)
            return fail("'stopPrice' must be a positive number");
    }

    return {Order(orderID, side, price, static_cast<std::uint32_t>(quantity), type, stopPrice), {}};
}

// ---- response building ------------------------------------------------------

json tradesToJson(const std::vector<Trade>& trades) {
    json array = json::array();
    for (const Trade& t : trades) {
        array.push_back({{"buyOrderID", t.buyOrderID},
                         {"sellOrderID", t.sellOrderID},
                         {"tradedPrice", t.tradedPrice},
                         {"tradedQuantity", t.tradedQuantity}});
    }
    return array;
}

json reportToJson(const ExecutionReport& r) {
    json body;
    body["orderID"] = r.orderID;
    body["status"] = std::string(toString(r.status));
    body["originalQuantity"] = r.originalQuantity;
    body["executedQuantity"] = r.executedQuantity;
    body["remainingQuantity"] = r.remainingQuantity;
    body["price"] = r.price;
    body["trades"] = tradesToJson(r.trades);
    if (r.triggered) body["triggered"] = true;
    if (!r.triggeredOrders.empty()) {
        json triggered = json::array();
        for (const ExecutionReport& t : r.triggeredOrders) triggered.push_back(reportToJson(t));
        body["triggeredOrders"] = std::move(triggered);
    }
    return body;
}

int statusFor(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::DuplicateOrderID: return 409;
        case RejectReason::UnknownOrder:     return 404;
        default:                             return 400;
    }
}

HttpResponse rejection(const ExecutionReport& r) {
    json body;
    body["orderID"] = r.orderID;
    body["status"] = std::string(toString(r.status));
    body["error"] = std::string(toString(r.reason));
    return {statusFor(r.reason), body.dump()};
}

}  // namespace

// ---------------------------------------------------------------------------

struct ApiServer::Impl {
    httplib::Server server;
};

ApiServer::ApiServer(MatchingEngine& engine, ApiServerConfig config)
    : engine_(engine), impl_(std::make_unique<Impl>()) {
    httplib::Server& server = impl_->server;

    const std::size_t workers = std::max<std::size_t>(config.workerThreads, 1);
    server.new_task_queue = [workers] { return new httplib::ThreadPool(workers); };
    server.set_tcp_nodelay(true);                    // do not delay small responses
    server.set_keep_alive_max_count(1u << 30);       // reuse connections for the whole session
    server.set_keep_alive_timeout(30);
    server.set_payload_max_length(64 * 1024);        // an order is a few hundred bytes
    server.Post("/api/v1/orders", [this](const httplib::Request& req, httplib::Response& res) {
        HttpResponse out = handleSubmit(req.body);
        res.status = out.status;
        res.set_content(std::move(out.body), kJson);
    });

    server.Delete("/api/v1/orders/:orderID", [this](const httplib::Request& req, httplib::Response& res) {
        HttpResponse out = handleCancel(req.path_params.at("orderID"));
        res.status = out.status;
        res.set_content(std::move(out.body), kJson);
    });

    // Uniform JSON errors for unmatched routes / methods / oversized payloads.
    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty()) {
            res.set_content(errorResponse(res.status, httplib::status_message(res.status)).body, kJson);
        }
    });
    server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
        res.status = 500;
        res.set_content(errorResponse(500, "internal error").body, kJson);
    });
}

ApiServer::~ApiServer() {
    stop();
}

int ApiServer::bind(const std::string& host, int port) {
    if (port == 0) return impl_->server.bind_to_any_port(host);
    return impl_->server.bind_to_port(host, port) ? port : -1;
}

bool ApiServer::run() { return impl_->server.listen_after_bind(); }

void ApiServer::stop() { impl_->server.stop(); }

HttpResponse ApiServer::handleSubmit(std::string_view body) {
    // Parse outside the engine lock.
    const json doc = json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return errorResponse(400, "malformed JSON");

    ParseResult parsed = parseOrder(doc);
    if (!parsed.order) return errorResponse(400, parsed.error);

    const ExecutionReport report = engine_.submit(*parsed.order);   // lock held only inside
    if (report.status == OrderStatus::REJECTED) return rejection(report);

    return {201, reportToJson(report).dump()};
}

HttpResponse ApiServer::handleCancel(std::string_view orderIDText) {
    std::uint64_t orderID = 0;
    const char* const first = orderIDText.data();
    const char* const last = first + orderIDText.size();
    const auto [end, ec] = std::from_chars(first, last, orderID);
    if (orderIDText.empty() || ec != std::errc{} || end != last)
        return errorResponse(400, "orderID must be an unsigned integer");

    const ExecutionReport report = engine_.cancel(orderID);
    if (report.status == OrderStatus::REJECTED) {
        json body;
        body["orderID"] = orderID;
        body["error"] = "order not found";
        return {404, body.dump()};
    }

    json body;
    body["orderID"] = report.orderID;
    body["status"] = std::string(toString(report.status));
    body["remainingQuantity"] = report.remainingQuantity;
    return {200, body.dump()};
}
