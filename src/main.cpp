#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include "api_server.hpp"
#include "matching_engine.hpp"

namespace {

void printUsage(const char* program) {
    std::cerr << "Usage: " << program << " [--host ADDR] [--port N] [--threads N]\n"
              << "  --host     bind address          (default 0.0.0.0)\n"
              << "  --port     TCP port              (default 8080)\n"
              << "  --threads  HTTP worker threads   (default 16)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::size_t threads = 16;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--host" && hasValue)         host = argv[++i];
        else if (arg == "--port" && hasValue)    port = std::atoi(argv[++i]);
        else if (arg == "--threads" && hasValue) threads = static_cast<std::size_t>(std::atoi(argv[++i]));
        else { printUsage(argv[0]); return arg == "--help" ? 0 : 2; }
    }
    if (port < 0 || port > 65535 || threads == 0) { printUsage(argv[0]); return 2; }

    // Block SIGINT/SIGTERM in every thread (inherited by the workers created
    // below) and handle them synchronously on one dedicated thread. This
    // avoids calling non-async-signal-safe code from a signal handler.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    MatchingEngine engine;
    ApiServer server(engine, ApiServerConfig{.workerThreads = threads});

    const int boundPort = server.bind(host, port);
    if (boundPort < 0) {
        std::cerr << "failed to bind " << host << ':' << port << '\n';
        return 1;
    }
    std::cout << "order-matching-engine listening on " << host << ':' << boundPort << std::endl;

    std::thread signalWatcher([&] {
        int received = 0;
        sigwait(&signals, &received);
        server.stop();
    });

    server.run();   // blocks until stop()

    // Wake the watcher if run() returned for a reason other than a signal. The
    // signal is blocked everywhere, so it simply stays pending if the watcher
    // has already consumed one; either way join() cannot hang.
    kill(getpid(), SIGTERM);
    signalWatcher.join();
    std::cout << "shut down cleanly" << std::endl;
    return 0;
}
