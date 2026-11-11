// A real StaticPoolServer fixture. The driver owns process-failure injection;
// this executable uses only the public lifecycle/capacity interface.
#include "host/static_pool_server.h"
#include "win32_compat.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct ReadyPair {
    int fd[2] = {-1, -1};
    ReadyPair() { require(capsid::win32::create_socket_pair(fd), "create READY pair"); }
    ~ReadyPair() {
        close(fd[0]);
        close(fd[1]);
    }
};

std::uint16_t read_port(int fd) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (line.find('\n') == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline, "READY deadline");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "poll READY");
        if (!polled)
            continue;
        char bytes[512];
        const auto count = capsid::win32::read_fd(fd, bytes, sizeof(bytes));
        require(count > 0, "read READY");
        line.append(bytes, static_cast<std::size_t>(count));
    }
    const std::string marker = "\"port\":";
    const auto position = line.find(marker);
    require(position != std::string::npos, "READY port field");
    const auto port = std::stoul(line.substr(position + marker.size()));
    require(port > 0 && port <= 65535, "READY port range");
    return static_cast<std::uint16_t>(port);
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected actual Worker path");
        ReadyPair ready;
        capsid::host::StaticPoolServerOptions options;
        options.workers = 2;
        auto &worker = options.worker_options;
        worker.worker_path = argv[1];
        worker.source_bundle_path = "static-pool-faults-inline";
        worker.source_name = "file://orders/v1/bundle.mjs";
        worker.application = "orders";
        worker.listen_address = "127.0.0.1";
        worker.listen_port = 0;
        worker.public_scheme = "http";
        worker.public_authority = "public.example";
        worker.request_timeout_ms = 6000;
        worker.initial_stream_window = 64U * 1024U;
        worker.strict_sandbox = false;
        worker.install_process_signals = false;
        worker.ready_fd = ready.fd[1];
        worker.cors.configured = true;
        worker.cors.allowed_origins = {"https://client.example"};
        worker.cors.allowed_methods = {"GET", "HEAD"};
        const std::string source = R"js(
const instance = crypto.randomUUID();
export default {fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/ws')) {
        const pair = new WebSocketPair();
        pair[1].accept();
        pair[1].binaryType = 'arraybuffer';
        pair[1].onmessage = event => pair[1].send(event.data);
        return new Response(null, {status: 101, webSocket: pair[0]});
    }
    return new Response(JSON.stringify({challenge: url.searchParams.get('challenge'), instance}));
}};
)js";
        capsid::host::StaticPoolServer pool(std::move(options));
        std::string error;
        require(pool.start(std::vector<std::uint8_t>(source.begin(), source.end()), &error),
                "start actual pool: " + error);
        require(pool.active_workers() == 2, "two live Workers at READY");
        // No HTTP probe: on Windows the first pending accept still belongs
        // to the first-started shard when the driver observes this record.
        std::cout << "{\"stage\":\"pool-ready\",\"active\":2,\"port\":" << read_port(ready.fd[0])
                  << "}" << std::endl;
        bool capacity_observed = false;
        bool zero_observed = false;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (!command.empty() && command.back() == '\r')
                command.pop_back();
            if (command == "await-one-exit") {
                require(!capacity_observed, "one capacity barrier");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
                while (pool.active_workers() != 1 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                require(pool.active_workers() == 1, "actual exit reached the shard reactor");
                capacity_observed = true;
                std::cout << "{\"stage\":\"one-worker-left\",\"active\":1}" << std::endl;
            } else if (command == "await-all-exits") {
                require(!zero_observed, "one zero-capacity barrier");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
                while (pool.active_workers() != 0 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                require(pool.active_workers() == 0, "all exits reached the shard reactors");
                zero_observed = true;
                std::cout << "{\"stage\":\"no-workers-left\",\"active\":0}" << std::endl;
            } else {
                require(command == "stop", "known driver command");
                break;
            }
        }
        pool.request_stop();
        require(pool.wait(&error), "join actual pool: " + error);
        require(pool.active_workers() == 0, "zero capacity after wait");
        std::cout << "{\"stage\":\"stopped\",\"active\":0}" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "static pool fixture: " << error.what() << std::endl;
        return 1;
    }
}
