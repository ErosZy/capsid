// Linux cold-start probe over the public capsid_worker C ABI. A JSON line
// requires exact first/warm responses and stable positive worker-tree PSS.
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include "capsid/runtime.h"
#include "cold_start_measurement.h"

namespace {
namespace cold = capsid::bench::cold;
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t interrupted = 0;
extern "C" void interrupt_handler(int number) { interrupted = number; }
void check_interrupt() {
    if (interrupted) cold::fail("interrupted", "measurement interrupted");
}
struct Args {
    std::string worker, mode, input, source_name;
    int iterations = 1, warm = 50, timeout_ms = 10000;
    int settle_ms = 1000, pss_interval_ms = 1000, pss_attempts = 8;
};
Args parse_args(int argc, char** argv) {
    Args out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) cold::fail("argument", "missing value for " + key);
            return argv[++i];
        };
        if (key == "--worker") out.worker = value();
        else if (key == "--mode") out.mode = value();
        else if (key == "--input") out.input = value();
        else if (key == "--source-name") out.source_name = value();
        else if (key == "--iterations") out.iterations = cold::bounded_count(value(), 1, 10000, "iterations");
        else if (key == "--warm") out.warm = cold::bounded_count(value(), 0, 100000, "warm requests");
        else if (key == "--timeout-ms") out.timeout_ms = cold::bounded_count(value(), 10, 60000, "timeout ms");
        else if (key == "--settle-ms") out.settle_ms = cold::bounded_count(value(), 0, 60000, "settle ms");
        else if (key == "--pss-interval-ms") out.pss_interval_ms = cold::bounded_count(value(), 1, 10000, "PSS interval ms");
        else if (key == "--pss-attempts") out.pss_attempts = cold::bounded_count(value(), 2, 64, "PSS attempts");
        else if (key != "--strict") cold::fail("argument", "unknown argument " + key);
    }
    if (out.worker.empty() || out.input.empty() || out.source_name.empty() || (out.mode != "source" && out.mode != "bytecode")) {
        cold::fail("argument", "required: --worker W --mode source|bytecode --input FILE --source-name NAME");
    }
    return out;
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) cold::fail("file-read", "cannot open " + path.string());
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) cold::fail("file-read", "cannot read " + path.string());
    return text;
}
class ProcFS {
public:
    cold::Identity identity(int64_t pid) const { return cold::parse_stat(pid, read_file(path(pid, "stat"))); }
    cold::Processes snapshot() const {
        cold::Processes processes;
        for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
            const uint64_t number = cold::unsigned_decimal(name, "process PID");
            if (number == 0 || number > static_cast<uint64_t>(INT64_MAX)) continue;
            // Foreign processes may disappear or be hidden. Missing pinned
            // members still fail tree()/identity() explicitly.
            std::ifstream in(entry.path() / "stat");
            if (!in) continue;
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (in.bad()) continue;
            const auto pid = static_cast<int64_t>(number);
            processes.emplace(pid, cold::parse_stat(pid, text));
        }
        return processes;
    }
    cold::Memory memory(int64_t pid) const { return cold::parse_smaps(read_file(path(pid, "smaps_rollup"))); }
private:
    static std::filesystem::path path(int64_t pid, const char* file) {
        return std::filesystem::path("/proc") / std::to_string(pid) / file;
    }
};
double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void wait_fd(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        check_interrupt();
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) cold::fail("timeout", "worker operation exceeded its deadline");
        struct pollfd pfd = {fd, events, 0};
        const int rc = poll(&pfd, 1, static_cast<int>(left));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) cold::fail("timeout", "worker event source did not become ready");
        if (pfd.revents & POLLNVAL) cold::fail("worker-fd", "invalid worker event source");
        return;  // next_event/flush diagnoses HUP/ERR through the C ABI.
    }
}
void drain_flush(capsid_worker* worker, int fd, Clock::time_point deadline) {
    for (;;) {
        check_interrupt();
        if (Clock::now() >= deadline) cold::fail("timeout", "write deadline exceeded");
        const auto result = capsid_worker_flush(worker);
        if (result == CAPSID_OK) return;
        if (result != CAPSID_WOULD_BLOCK) cold::fail("flush", capsid_result_string(result));
        wait_fd(fd, POLLOUT, deadline);
    }
}
capsid_event next_event(capsid_worker* worker, int fd, Clock::time_point deadline) {
    for (;;) {
        check_interrupt();
        if (Clock::now() >= deadline) cold::fail("timeout", "event deadline exceeded");
        capsid_event event{};
        event.struct_size = sizeof(event);
        const auto result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_EXIT || event.type == CAPSID_EVENT_ERROR || event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                cold::fail("worker-event", "worker failed before validated response");
            }
            return event;
        }
        if (result != CAPSID_WOULD_BLOCK) cold::fail("next-event", capsid_result_string(result));
        wait_fd(fd, POLLIN, deadline);
    }
}
void wait_ready(capsid_worker* worker, int fd, Clock::time_point deadline) {
    for (;;) {
        const auto event = next_event(worker, fd, deadline);
        if (event.type == CAPSID_EVENT_READY) return;
        if (event.type != CAPSID_EVENT_LOG && event.type != CAPSID_EVENT_AUDIT) cold::fail("ready-order", "unexpected event before READY");
    }
}
double issue_request(capsid_worker* worker, int fd, uint64_t id, int timeout_ms) {
    const auto started = Clock::now(), deadline = started + std::chrono::milliseconds(timeout_ms);
    const auto begin = capsid_worker_begin_bodyless_request(worker, id, "GET", "/@capsid/cold-start", nullptr, 0);
    if (begin != CAPSID_OK) cold::fail("request-begin", capsid_result_string(begin));
    drain_flush(worker, fd, deadline);
    cold::ResponseCheck response(id);
    for (;;) {
        const auto event = next_event(worker, fd, deadline);
        switch (event.type) {
        case CAPSID_EVENT_RESPONSE_HEAD:
            response.head(event.request_id, event.status, (event.flags & CAPSID_RESPONSE_HEAD_FLAG_UPGRADE) != 0);
            break;
        case CAPSID_EVENT_RESPONSE_BODY:
            if (event.payload.size && !event.payload.data) cold::fail("response-body", "null response bytes");
            response.body(event.request_id, std::string_view(event.payload.size ? reinterpret_cast<const char*>(event.payload.data) : "", event.payload.size));
            break;
        case CAPSID_EVENT_RESPONSE_END:
            response.end(event.request_id);
            return ms_since(started);
        case CAPSID_EVENT_LOG:
        case CAPSID_EVENT_AUDIT:
        case CAPSID_EVENT_MEMORY_METRICS: break;
        case CAPSID_EVENT_REQUEST_CREDIT:
            if (event.request_id != id) cold::fail("response-order", "credit for another request");
            break;
        default: cold::fail("response-order", "unexpected event during response");
        }
    }
}
void sleep_checked(int milliseconds) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (Clock::now() < end) {
        check_interrupt();
        std::this_thread::sleep_for(std::min(end - Clock::now(), std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(20))));
    }
    check_interrupt();
}
void emit_sample(FILE* out, const cold::Sample& sample) {
    std::fprintf(out, "{\"pss_kb\":%llu,\"rss_kb\":%llu,\"processes\":[", static_cast<unsigned long long>(sample.pss), static_cast<unsigned long long>(sample.rss));
    for (size_t i = 0; i < sample.parts.size(); ++i) {
        const auto& part = sample.parts[i];
        std::fprintf(out, "%s{\"pid\":%lld,\"start\":%llu,\"pss_kb\":%llu,\"rss_kb\":%llu}", i ? "," : "", static_cast<long long>(part.identity.pid),
            static_cast<unsigned long long>(part.identity.start), static_cast<unsigned long long>(part.memory.pss), static_cast<unsigned long long>(part.memory.rss));
    }
    std::fprintf(out, "]}");
}
void run(const Args& args) {
    const std::string input = read_file(args.input);
    if (input.empty()) cold::fail("input-empty", "fixture cannot be empty");
    ProcFS proc;
    for (int iter = 0; iter < args.iterations; ++iter) {
        check_interrupt();
        const auto t0 = Clock::now(), startup_deadline = t0 + std::chrono::milliseconds(args.timeout_ms);
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = args.worker.c_str();
        config.strict_sandbox = 1;
        capsid_worker* raw = nullptr;
        const auto spawned = capsid_worker_spawn(&config, &raw);
        std::unique_ptr<capsid_worker, decltype(&capsid_worker_destroy)> worker(raw, capsid_worker_destroy);
        if (spawned != CAPSID_OK || !worker) cold::fail("spawn", capsid_result_string(spawned));
        const double spawn_ms = ms_since(t0);
        // The public API reports the spawned child. SO_PEERCRED on the
        // inherited socketpair identifies the socket creator instead.
        const int64_t worker_pid = capsid_worker_pid(worker.get());
        if (worker_pid <= 0 || worker_pid == static_cast<int64_t>(getpid())) cold::fail("worker-pid", "invalid spawned worker PID");
        const auto identity = proc.identity(worker_pid);
        const auto* bytes = reinterpret_cast<const uint8_t*>(input.data());
        const auto loaded = args.mode == "bytecode"
            ? capsid_worker_load_trusted_bytecode_named(worker.get(), bytes, input.size(), args.source_name.c_str())
            : capsid_worker_load_bundle_named(worker.get(), bytes, input.size(), args.source_name.c_str());
        if (loaded != CAPSID_OK) cold::fail("load", capsid_result_string(loaded));
        const int fd = capsid_worker_fd(worker.get());
        drain_flush(worker.get(), fd, startup_deadline);
        const double load_ms = ms_since(t0);
        wait_ready(worker.get(), fd, startup_deadline);
        const double ready_ms = ms_since(t0);
        const double first_request_ms = issue_request(worker.get(), fd, 1, args.timeout_ms);
        const double first_ms = ms_since(t0);
        // Stop timing before membership discovery, warm-up, settle or PSS.
        const auto pinned = cold::tree(identity, proc.snapshot());
        std::vector<double> latencies;
        for (int i = 0; i < args.warm; ++i) latencies.push_back(issue_request(worker.get(), fd, static_cast<uint64_t>(i) + 2, args.timeout_ms));
        double warm_ms = 0;
        if (!latencies.empty()) {
            auto ordered = latencies;
            std::sort(ordered.begin(), ordered.end());
            warm_ms = ordered[ordered.size() / 2];
            if (ordered.size() % 2 == 0) warm_ms = (warm_ms + ordered[ordered.size() / 2 - 1]) / 2;
        }
        sleep_checked(args.settle_ms);
        const auto samples = cold::stable_samples([&]() { check_interrupt(); return cold::sample_tree(proc, identity, pinned); },
            [&]() { sleep_checked(args.pss_interval_ms); }, [&](const cold::Sample& sample) {
                std::fprintf(stderr, "pss-attempt "); emit_sample(stderr, sample); std::fputc('\n', stderr);
            }, args.pss_attempts);
        worker.reset();  // Abortive C ABI destroy reaps this child before output.
        check_interrupt();
        const auto& last = samples.back();
        std::printf("{\"round\":%d,\"ok\":true,\"mode\":\"%s\",\"size\":%zu,\"spawn_ms\":%.6f,\"load_ms\":%.6f,"
                    "\"ready_ms\":%.6f,\"first_ms\":%.6f,\"total_ms\":%.6f,\"first_request_ms\":%.6f,"
                    "\"warm_ms\":%.6f,\"warm_n\":%d,\"worker_pid\":%lld,\"pss_kb\":%llu,\"worker_pss_kb\":%llu,"
                    "\"worker_rss_kb\":%llu,\"pss_readings_kb\":[%llu,%llu],\"pssSamples\":[",
            iter + 1, args.mode.c_str(), input.size(), spawn_ms, load_ms, ready_ms, first_ms, first_ms, first_request_ms, warm_ms, args.warm,
            static_cast<long long>(worker_pid), static_cast<unsigned long long>(last.pss), static_cast<unsigned long long>(last.pss), static_cast<unsigned long long>(last.rss),
            static_cast<unsigned long long>(samples[samples.size() - 2].pss), static_cast<unsigned long long>(last.pss));
        emit_sample(stdout, samples[samples.size() - 2]); std::fputc(',', stdout); emit_sample(stdout, last);
        std::printf("],\"pssAttempts\":%zu,\"measurementProcesses\":[", samples.size());
        size_t index = 0;
        for (const auto& [pid, member] : pinned) std::printf("%s{\"pid\":%lld,\"start\":%llu}", index++ ? "," : "", static_cast<long long>(pid), static_cast<unsigned long long>(member.start));
        std::printf("],\"membershipPinnedAfter\":\"first-response-before-warm\",\"warm_latencies_ms\":[");
        for (size_t i = 0; i < latencies.size(); ++i) std::printf("%s%.6f", i ? "," : "", latencies[i]);
        std::printf("],\"correctness\":{\"ok\":true,\"responses_checked\":%d,\"errors\":0,\"timeouts\":0,\"mismatches\":0,\"errors_as_success\":false},"
                    "\"posture\":\"c-abi-strict\",\"cleanupScope\":\"c-abi-worker\"}\n", args.warm + 1);
        std::fflush(stdout);
    }
}
}  // namespace
int main(int argc, char** argv) {
    struct sigaction action{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) return 1;
    try { run(parse_args(argc, argv)); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "cold-start: %s\n", error.what()); return 1; }
}
