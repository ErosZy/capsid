#ifndef CAPSID_HOST_PROCESS_SNAPSHOT_H
#define CAPSID_HOST_PROCESS_SNAPSHOT_H

#include "host/metrics.h"

#include <atomic>
#include <sys/types.h>

namespace capsid::host {

// M2 item 7 (design §12.1 "process family"): the default render-time
// snapshot provider. Reads the Host's own RSS/PSS from /proc/self and its
// CPU time from /proc/self/stat; reads the currently active worker's RSS
// from /proc/<pid>/statm.
//
// worker_pid is a process-shared atomic the Host keeps updated when a
// worker activates (managed mode: at activation; single-worker/static-pool:
// at spawn). A zero pid renders a zero worker series — never fabricated
// data from an unknown process. The provider is called on the /metrics
// render path only, so its /proc reads never block the counter methods.
MetricsRegistry::ProcessSnapshotProvider default_process_snapshot_provider(
    const std::atomic<pid_t>* worker_pid);

}  // namespace capsid::host

#endif
