// Cold-start measurement tool: process start -> spawn -> load -> READY ->
// first response, over the public capsid_worker C ABI.
//
//   cold-start --worker <capsid-worker> --mode source|bytecode \
//              --input <bundle.mjs|bundle.qjsb> --source-name <file://...> \
//              [--iterations N]
//
// Emits one JSON line per iteration (phases in ms from process start):
//   {"mode":..., "size":..., "spawn_ms":..., "load_ms":..., "ready_ms":...,
//    "first_ms":..., "total_ms":...}
//
// Source and trusted-bytecode paths exercise the same spawn/handshake/flush
// machinery; only the load call differs (load_bundle_named vs
// load_trusted_bytecode_named). READY and first-response are observed from
// the event stream, so the numbers include worker-side parse/compile for
// source mode and bytecode deserialization for bytecode mode.
#include <poll.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "capsid/runtime.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
    std::string worker;
    std::string mode;  // source | bytecode
    std::string input;
    std::string source_name;
    int iterations = 1;
};

Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (key == "--worker") out.worker = value("--worker");
        else if (key == "--mode") out.mode = value("--mode");
        else if (key == "--input") out.input = value("--input");
        else if (key == "--source-name") out.source_name = value("--source-name");
        else if (key == "--iterations") out.iterations = std::stoi(value("--iterations"));
        else {
            std::fprintf(stderr, "unknown argument: %s\n", key.c_str());
            std::exit(2);
        }
    }
    if (out.worker.empty() || out.input.empty() || out.source_name.empty() ||
        (out.mode != "source" && out.mode != "bytecode")) {
        std::fprintf(stderr,
                     "usage: cold-start --worker W --mode source|bytecode "
                     "--input FILE --source-name NAME [--iterations N]\n");
        std::exit(2);
    }
    return out;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    const std::streamsize size = in.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    return bytes;
}

// Drain events until `wanted` arrives. Returns false on worker exit or error.
bool wait_for_event(capsid_worker* worker, int fd, capsid_event_type wanted) {
    for (;;) {
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result next = capsid_worker_next_event(worker, &event);
        if (next == CAPSID_OK) {
            if (event.type == wanted) return true;
            if (event.type == CAPSID_EVENT_EXIT || event.type == CAPSID_EVENT_ERROR) {
                std::fprintf(stderr, "worker exited before event %d\n", wanted);
                return false;
            }
            continue;
        }
        if (next == CAPSID_WOULD_BLOCK) {
            struct pollfd pfd = {fd, POLLIN, 0};
            const int rc = poll(&pfd, 1, 10000);
            if (rc <= 0) {
                std::fprintf(stderr, "poll timeout waiting for event %d\n", wanted);
                return false;
            }
            continue;
        }
        std::fprintf(stderr, "next_event: %s\n", capsid_result_string(next));
        return false;
    }
}

// capsid_worker_flush writes as much as the socket accepts and returns
// WOULD_BLOCK once the send buffer is full; the caller must retry on
// POLLOUT. Large bundles (1 MB) exceed the send buffer in one write, so
// drain the whole write buffer before waiting for any inbound event.
bool drain_flush(capsid_worker* worker, int fd) {
    capsid_result result = capsid_worker_flush(worker);
    while (result == CAPSID_WOULD_BLOCK) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        const int rc = poll(&pfd, 1, 10000);
        if (rc <= 0) {
            std::fprintf(stderr, "poll timeout draining write buffer\n");
            return false;
        }
        result = capsid_worker_flush(worker);
    }
    if (result != CAPSID_OK) {
        std::fprintf(stderr, "flush: %s\n", capsid_result_string(result));
        return false;
    }
    return true;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const std::vector<uint8_t> input = read_file(args.input);

    for (int iter = 0; iter < args.iterations; ++iter) {
        const auto t0 = Clock::now();

        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = args.worker.c_str();

        capsid_worker* worker = nullptr;
        if (capsid_worker_spawn(&config, &worker) != CAPSID_OK || worker == nullptr) {
            std::fprintf(stderr, "spawn failed\n");
            return 1;
        }
        const double spawn_ms = ms_since(t0);

        const capsid_result load_result =
            args.mode == "bytecode"
                ? capsid_worker_load_trusted_bytecode_named(
                      worker, input.data(), input.size(), args.source_name.c_str())
                : capsid_worker_load_bundle_named(
                      worker, input.data(), input.size(), args.source_name.c_str());
        if (load_result != CAPSID_OK) {
            std::fprintf(stderr, "load %s: %s\n", args.mode.c_str(),
                         capsid_result_string(load_result));
            capsid_worker_destroy(worker);
            return 1;
        }
        const int fd = capsid_worker_fd(worker);
        if (!drain_flush(worker, fd)) {
            capsid_worker_destroy(worker);
            return 1;
        }
        const double load_ms = ms_since(t0);

        if (!wait_for_event(worker, fd, CAPSID_EVENT_READY)) {
            capsid_worker_destroy(worker);
            return 1;
        }
        const double ready_ms = ms_since(t0);

        const char* kPath = "/@capsid/cold-start";
        if (capsid_worker_begin_bodyless_request(worker, 1, "GET", kPath,
                                                 nullptr, 0) != CAPSID_OK ||
            !drain_flush(worker, fd)) {
            std::fprintf(stderr, "begin request failed\n");
            capsid_worker_destroy(worker);
            return 1;
        }
        if (!wait_for_event(worker, fd, CAPSID_EVENT_RESPONSE_END)) {
            capsid_worker_destroy(worker);
            return 1;
        }
        const double first_ms = ms_since(t0);

        capsid_worker_destroy(worker);

        std::printf(
            "{\"mode\":\"%s\",\"size\":%zu,\"spawn_ms\":%.2f,\"load_ms\":%.2f,"
            "\"ready_ms\":%.2f,\"first_ms\":%.2f,\"total_ms\":%.2f}\n",
            args.mode.c_str(), input.size(), spawn_ms, load_ms, ready_ms,
            first_ms, first_ms);
    }
    return 0;
}
