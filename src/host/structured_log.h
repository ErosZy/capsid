#ifndef CAPSID_HOST_STRUCTURED_LOG_H
#define CAPSID_HOST_STRUCTURED_LOG_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace capsid::host {

// M2 item 7 (design §12.2): every Host log line is a single JSON object
// with the fixed field set — timestamp, level, event, app, version,
// generation, worker_id, request_id, operation_id, stage, result,
// duration_ms — plus "binding", "fields", and a "message" field for
// sanitized Binding/application output. `fields` is a pre-validated JSON
// object and is emitted as an object, never as an escaped JSON string.
//
// Field-default rule: every string field appears only when it is non-empty
// (an event carries the fields it concerns); duration_ms always appears
// (0 is a legal value). timestamp, level and event always appear.
//
// Forbidden content (never a field value): secret values, Authorization /
// Cookie header values, raw request/response bodies, unsanitized
// application errors.
//
// Two lanes (§12.2): the app lane carries runtime LOG/AUDIT forwarding —
// bounded, drops on overflow and counts the drop. The control lane carries
// deploy/security/process-lifecycle events — bounded and NEVER dropped: an
// overflowing control lane writes through to the sink synchronously, so
// CRASH_BUDGET_EXCEEDED, quarantine/retired transitions, retire drain
// timeouts and admin authorization failures always land. A slow sink can
// therefore stall the caller only through control events (rare, and the
// semantics of "never dropped"); the reactor-facing app path stays bounded.

struct LogFields {
    // Every member has a default initializer so designated initializers
    // ({.event = ..., .app = ...}) remain complete under
    // -Wmissing-field-initializers; the field-default rule (non-empty
    // strings appear, empty ones are omitted) is applied at encode time.
    std::string level = "info";  // debug | info | warn | error
    std::string event = {};      // one of the fixed names below
    std::string app = {};
    std::string version = {};
    std::string generation = {};
    std::string binding = {};
    std::string worker_id = {};
    std::string request_id = {};
    std::string operation_id = {};
    std::string stage = {};
    std::string result = {};
    std::string fields = {};  // empty, or a validated JSON object
    std::uint64_t duration_ms = 0;
    std::string message = {};  // static or sanitized text only
};

enum class LogLane {
    kApp,      // runtime LOG/AUDIT forwarding: bounded, droppable, counted
    kControl,  // deploy/security/process lifecycle: bounded, never dropped
};

// Fixed event vocabulary (controlled — the "event" field only ever holds
// one of these names).
namespace log_events {
inline constexpr char kStartup[] = "startup";
inline constexpr char kShutdown[] = "shutdown";
inline constexpr char kDeployStage[] = "deploy_stage";
inline constexpr char kWorkerStarting[] = "worker_starting";
inline constexpr char kWorkerReady[] = "worker_ready";
inline constexpr char kWorkerBusy[] = "worker_busy";
inline constexpr char kWorkerUnhealthy[] = "worker_unhealthy";
inline constexpr char kWorkerCrash[] = "worker_crash";
inline constexpr char kWorkerReplaced[] = "worker_replaced";
inline constexpr char kWorkerDraining[] = "worker_draining";
inline constexpr char kRecoveryDecision[] = "recovery_decision";
inline constexpr char kHealthProbe[] = "health_probe";
inline constexpr char kQuarantine[] = "quarantine";
inline constexpr char kRetire[] = "retire";
inline constexpr char kAdminAuth[] = "admin_auth";
inline constexpr char kAdminRequest[] = "admin_request";
inline constexpr char kAppLog[] = "app_log";
inline constexpr char kAppAudit[] = "app_audit";
inline constexpr char kLogDrop[] = "log_drop";
inline constexpr char kWorkerCommandError[] = "worker_command_error";
}  // namespace log_events

// Single-writer structured log. log() is thread-safe; one writer thread
// drains both FIFO lanes in order (control lane first when both non-empty,
// so a control event never waits behind a backlog of app events).
class StructuredLog {
public:
    using Sink = std::function<void(const std::string& line)>;

    StructuredLog(Sink sink,
                  std::size_t app_capacity = 1024,
                  std::size_t control_capacity = 256);
    ~StructuredLog();

    StructuredLog(const StructuredLog&) = delete;
    StructuredLog& operator=(const StructuredLog&) = delete;

    // Thread-safe. Control events never drop (overflow writes through to
    // the sink synchronously). App events drop on overflow and count.
    void log(LogLane lane, LogFields fields);

    // Drains both lanes, joins the writer thread. Idempotent; the
    // destructor calls it as well.
    void flush();

    // M2 item 7 metrics input: app-lane events dropped since construction.
    std::uint64_t dropped_app_events() const;
    // Events accepted (enqueued or written through) since construction.
    std::uint64_t accepted_events() const;

private:
    void writer_run();
    void write_through(LogFields fields);

    Sink sink_;
    std::size_t app_capacity_;
    std::size_t control_capacity_;
    std::thread writer_;
    std::atomic<bool> stop_ = false;
    std::atomic<std::uint64_t> dropped_app_ = 0;
    std::atomic<std::uint64_t> accepted_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable queue_ready_;
    std::deque<LogFields> app_queue_;
    std::deque<LogFields> control_queue_;
};

// Encodes one fixed-field JSON line (exported for tests). timestamp_ms is
// the epoch-millisecond value placed in "timestamp".
std::string encode_log_line(const LogFields& fields,
                            std::uint64_t timestamp_ms);

}  // namespace capsid::host

#endif
