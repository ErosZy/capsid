#include "host/metrics.h"

namespace capsid::host {

namespace {

const char* label_escape(const std::string& value) {
    // Prometheus label values may not contain '"', '\\' or '\n'. All
    // label values here are controlled (app ids, digest hex, fixed
    // enums), but escape defensively: an app id cannot hold those
    // characters anyway, and a digest is pure hex.
    static thread_local std::string escaped;
    escaped.clear();
    for (const char c : value) {
        if (c == '"' || c == '\\' || c == '\n') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped.c_str();
}

}  // namespace

void MetricsRegistry::set_process_snapshot_provider(
    ProcessSnapshotProvider provider) {
    process_snapshot_ = std::move(provider);
}

void MetricsRegistry::count_worker_event(const std::string& event,
                                         const std::string& app,
                                         const std::string& generation) {
    std::string key = "worker_events_total{event=\"";
    key += label_escape(event);
    key += "\",app=\"";
    key += label_escape(app);
    key += "\",generation=\"";
    key += label_escape(generation);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::set_recovery_instability_budget_remaining(
    const std::string& app, std::uint64_t value) {
    std::string key = "recovery_instability_budget_remaining{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::move(key)] = value;
}

void MetricsRegistry::set_recovery_backoff_ms(const std::string& app,
                                              std::uint64_t value) {
    std::string key = "recovery_backoff_ms{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::move(key)] = value;
}

void MetricsRegistry::count_recovery_startup_permit_grant(
    const std::string& app) {
    std::string key = "recovery_startup_permits_total{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::count_recovery_quarantine(const std::string& app) {
    std::string key = "recovery_quarantines_total{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::count_recovery_retire(const std::string& app) {
    std::string key = "recovery_retires_total{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::count_deploy_stage(const std::string& stage,
                                         const std::string& result,
                                         const std::string& app) {
    std::string key = "deploy_stages_total{stage=\"";
    key += label_escape(stage);
    key += "\",result=\"";
    key += label_escape(result);
    key += "\",app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::count_deploy_operation(const std::string& result,
                                             const std::string& app) {
    std::string key = "deploy_operations_total{result=\"";
    key += label_escape(result);
    key += "\",app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::set_isolation_required_features(const std::string& app,
                                                      std::uint64_t value) {
    std::string key = "isolation_required_features{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::move(key)] = value;
}

void MetricsRegistry::set_isolation_applied_features(const std::string& app,
                                                     std::uint64_t value) {
    std::string key = "isolation_applied_features{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::move(key)] = value;
}

void MetricsRegistry::count_isolation_failure(const std::string& kind,
                                              const std::string& app) {
    std::string key = "isolation_failures_total{kind=\"";
    key += label_escape(kind);
    key += "\",app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_[std::move(key)];
}

void MetricsRegistry::set_log_dropped(const std::string& app,
                                      std::uint64_t value) {
    std::string key = "log_dropped_total{app=\"";
    key += label_escape(app);
    key += "\"}";
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[std::move(key)] = value;
}

std::string MetricsRegistry::render_counters(
    const std::map<std::string, std::uint64_t>& series) const {
    // series keys are "name{labels}" — the name is the text before '{'.
    // A "_total" suffix marks a counter; everything else is a gauge.
    std::string out;
    std::string current_name;
    for (const auto& [key, value] : series) {
        const std::string::size_type brace = key.find('{');
        const std::string name =
            brace == std::string::npos ? key : key.substr(0, brace);
        if (name != current_name) {
            current_name = name;
            const bool is_counter = name.find("_total") != std::string::npos;
            // The TYPE line must carry the same capsid_ prefix as the
            // series lines below, or the family name diverges.
            out += "# TYPE capsid_" + name + " " +
                   (is_counter ? "counter" : "gauge") + "\n";
        }
        out += "capsid_" + key + " " + std::to_string(value) + "\n";
    }
    return out;
}

std::string MetricsRegistry::render_prometheus_text() const {
    std::map<std::string, std::uint64_t> counters;
    std::map<std::string, std::uint64_t> gauges;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        counters = counters_;
        gauges = gauges_;
    }
    // Process family: host and worker RSS/PSS/CPU sampled at render time.
    if (process_snapshot_) {
        const ProcessMetricsSnapshot snapshot = process_snapshot_();
        counters["process_rss_bytes{}"] = snapshot.rss_bytes;
        counters["process_pss_bytes{}"] = snapshot.pss_bytes;
        counters["process_cpu_seconds_total{}"] =
            snapshot.cpu_seconds_total;
        gauges["worker_rss_bytes{app=\"\",pid=\"" +
               std::to_string(snapshot.worker_pid) + "\"}"] =
            snapshot.worker_rss_bytes;
    }
    // Deterministic render: counters first (fixed family order via the
    // sorted maps), then gauges.
    std::string out = render_counters(counters);
    out += render_counters(gauges);
    return out;
}

}  // namespace capsid::host
