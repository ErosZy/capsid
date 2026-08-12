#ifndef CAPSID_HOST_METRICS_H
#define CAPSID_HOST_METRICS_H

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace capsid::host {

// M2 item 7 (design §12.1): the fixed, low-cardinality metric registry
// behind the Admin /metrics endpoint. Families:
//
//   capsid_worker_events_total{event,app,generation}          counter
//   capsid_recovery_instability_budget_remaining{app}         gauge
//   capsid_recovery_backoff_ms{app}                           gauge
//   capsid_recovery_startup_permits_total{app}                counter
//   capsid_recovery_quarantines_total{app}                    counter
//   capsid_recovery_retires_total{app}                        counter
//   capsid_deploy_stages_total{stage,result,app}              counter
//   capsid_deploy_operations_total{result,app}                counter
//   capsid_isolation_required_features{app}                   gauge
//   capsid_isolation_applied_features{app}                    gauge
//   capsid_isolation_failures_total{kind,app}                 counter
//   capsid_log_dropped_total{app}                             counter
//   capsid_process_rss_bytes / capsid_process_pss_bytes /     gauge
//   capsid_process_cpu_seconds_total                          counter
//   capsid_worker_rss_bytes{app,pid}                          gauge
//
// Labels are controlled: app, generation (a digest), result and event
// enums only. Request IDs, URLs, version free text, hostnames and error
// messages never become labels. The request/latency/stream families of
// §12.1 are unreachable in managed mode: there is no data plane on the
// Host (requests terminate at an external gateway), so those families
// intentionally do not exist here. A process snapshot is injected by the
// Host at startup (render-time /proc reads, never blocking the caller of
// the count methods).
struct ProcessMetricsSnapshot {
    std::uint64_t rss_bytes = 0;
    std::uint64_t pss_bytes = 0;
    std::uint64_t cpu_seconds_total = 0;
    std::uint64_t worker_rss_bytes = 0;
    std::uint64_t worker_pid = 0;
};

class MetricsRegistry {
public:
    using ProcessSnapshotProvider =
        std::function<ProcessMetricsSnapshot()>;

    void set_process_snapshot_provider(ProcessSnapshotProvider provider);

    void count_worker_event(const std::string& event,
                            const std::string& app,
                            const std::string& generation);
    void set_recovery_instability_budget_remaining(const std::string& app,
                                                   std::uint64_t value);
    void set_recovery_backoff_ms(const std::string& app, std::uint64_t value);
    void count_recovery_startup_permit_grant(const std::string& app);
    void count_recovery_quarantine(const std::string& app);
    void count_recovery_retire(const std::string& app);
    void count_deploy_stage(const std::string& stage,
                            const std::string& result,
                            const std::string& app);
    void count_deploy_operation(const std::string& result,
                                const std::string& app);
    void set_isolation_required_features(const std::string& app,
                                         std::uint64_t value);
    void set_isolation_applied_features(const std::string& app,
                                        std::uint64_t value);
    void count_isolation_failure(const std::string& kind,
                                 const std::string& app);
    void set_log_dropped(const std::string& app, std::uint64_t value);

    // Prometheus text format, families in a fixed order, series sorted
    // within a family (deterministic output for scrapers and tests).
    std::string render_prometheus_text() const;

private:
    std::string render_counters(
        const std::map<std::string, std::uint64_t>& series) const;

    ProcessSnapshotProvider process_snapshot_;
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t> counters_;
    std::map<std::string, std::uint64_t> gauges_;
};

}  // namespace capsid::host

#endif
