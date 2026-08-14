#include "host/structured_log.h"

#include <chrono>

namespace capsid::host {

namespace {

// JSON string escaping for field values. Log values are already sanitized
// at the call site (secrets, headers, bodies and raw errors never arrive
// here); escaping keeps the single-line JSON contract even for unusual
// application payloads in the message field.
std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

}  // namespace

StructuredLog::StructuredLog(Sink sink,
                             std::size_t app_capacity,
                             std::size_t control_capacity)
    : sink_(std::move(sink)),
      app_capacity_(app_capacity),
      control_capacity_(control_capacity) {
    writer_ = std::thread([this] { writer_run(); });
}

StructuredLog::~StructuredLog() {
    flush();
}

void StructuredLog::flush() {
    if (!stop_.exchange(true)) {
        queue_ready_.notify_all();
        writer_.join();
    }
}

void StructuredLog::writer_run() {
    for (;;) {
        LogFields fields;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            queue_ready_.wait(lock, [this] {
                return stop_.load(std::memory_order_relaxed) ||
                       !control_queue_.empty() || !app_queue_.empty();
            });
            if (!control_queue_.empty()) {
                // Control first: a security/lifecycle event never waits
                // behind an app-log backlog.
                fields = std::move(control_queue_.front());
                control_queue_.pop_front();
                have = true;
            } else if (!app_queue_.empty()) {
                fields = std::move(app_queue_.front());
                app_queue_.pop_front();
                have = true;
            } else if (stop_.load(std::memory_order_relaxed)) {
                return;
            }
        }
        if (have) {
            const std::uint64_t timestamp_ms =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
            // §12.2: the newline is part of the output contract (one JSON
            // object per line); the sink receives a terminated line.
            sink_(encode_log_line(fields, timestamp_ms) + "\n");
        }
    }
}

void StructuredLog::write_through(LogFields fields) {
    // Control-lane overflow: the event is written synchronously so it can
    // never be dropped. Rare by construction (quarantine, retire, auth
    // failures); the cost is a blocking write on the caller's thread, which
    // is exactly the semantics of "control-plane events are not droppable".
    const std::uint64_t timestamp_ms =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    sink_(encode_log_line(fields, timestamp_ms) + "\n");
}

void StructuredLog::log(LogLane lane, LogFields fields) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepted_.fetch_add(1, std::memory_order_relaxed);
        if (lane == LogLane::kApp) {
            if (app_queue_.size() >= app_capacity_) {
                // Bounded app lane: drop and count. The writer never
                // blocks the reactor path; the log-drop metric feeds
                // /metrics from dropped_app_events().
                dropped_app_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            app_queue_.push_back(std::move(fields));
            queue_ready_.notify_one();
            return;
        }
        if (control_queue_.size() >= control_capacity_) {
            // Fall through to the synchronous write outside the lock.
        } else {
            control_queue_.push_back(std::move(fields));
            queue_ready_.notify_one();
            return;
        }
    }
    write_through(std::move(fields));
}

std::uint64_t StructuredLog::dropped_app_events() const {
    return dropped_app_.load(std::memory_order_relaxed);
}

std::uint64_t StructuredLog::accepted_events() const {
    return accepted_.load(std::memory_order_relaxed);
}

std::string encode_log_line(const LogFields& fields,
                            std::uint64_t timestamp_ms) {
    std::string line = "{\"timestamp\":\"";
    line += std::to_string(timestamp_ms);
    line += "\",\"level\":\"";
    line += json_escape(fields.level);
    line += "\",\"event\":\"";
    line += json_escape(fields.event);
    auto append_field = [&line](const char* name, const std::string& value) {
        if (!value.empty()) {
            line += "\",\"";
            line += name;
            line += "\":\"";
            line += json_escape(value);
        }
    };
    append_field("app", fields.app);
    append_field("version", fields.version);
    append_field("generation", fields.generation);
    append_field("worker_id", fields.worker_id);
    append_field("request_id", fields.request_id);
    append_field("operation_id", fields.operation_id);
    append_field("stage", fields.stage);
    append_field("result", fields.result);
    line += "\",\"duration_ms\":";
    line += std::to_string(fields.duration_ms);
    line += ",\"message\":\"";
    line += json_escape(fields.message);
    // encode_log_line is pure single-line serialization; the trailing
    // newline belongs to the OUTPUT contract (§12.2: one JSON object per
    // line, consumers parse line by line) and is appended at the sink
    // call sites, not here.
    line += "\"}";
    return line;
}

}  // namespace capsid::host
