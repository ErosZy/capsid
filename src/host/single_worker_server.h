#ifndef CAPSID_HOST_SINGLE_WORKER_SERVER_H
#define CAPSID_HOST_SINGLE_WORKER_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace capsid::host {

// Defined in single_worker_server.cc; the server keeps it behind a shared
// pointer so the header exposes no Boost or worker-Runtime types.
class Impl;

// M1A benchmark fixture configuration. Every field is validated by main.cc
// before the server is constructed.
struct SingleWorkerServerOptions {
    std::string worker_path;
    std::string source_bundle_path;
    std::string source_name;
    std::string application;
    std::string listen_address;
    std::uint16_t listen_port = 0;
    std::string public_scheme;
    std::string public_authority;
    std::uint64_t request_timeout_ms = 0;
    std::uint32_t initial_stream_window = 0;
    bool strict_sandbox = false;
    int ready_fd = -1;
};

// M1A single-worker Host data plane: one Boost.Asio io_context owner, one
// capsid_worker owned by a dedicated worker thread, Beast HTTP/1 as the sole
// framing authority, and M0 request normalization at the route boundary.
// This mode is an explicit benchmark fixture, not a deployment API: it never
// reads or writes active.json and must not be described as a production path.
class SingleWorkerServer {
public:
    explicit SingleWorkerServer(SingleWorkerServerOptions options);
    ~SingleWorkerServer();

    SingleWorkerServer(const SingleWorkerServer&) = delete;
    SingleWorkerServer& operator=(const SingleWorkerServer&) = delete;

    // Runs the spawn/READY/listen/ready-fd sequence, then the event loop,
    // until SIGTERM/SIGINT. Returns the process exit code.
    int run(const std::vector<std::uint8_t>& bundle);

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace capsid::host

#endif
