#include "host/process_snapshot.h"

#if defined(_WIN32)
#include <psapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

namespace capsid::host {

namespace {

#if defined(_WIN32)

// Reads the working-set size of a process via the process API (the
// Windows equivalent of /proc/<pid>/statm field 2). Returns false when
// the process is gone or the query fails; the caller renders a zero
// series, never a fabrication.
bool read_rss_kib(pid_t pid, std::uint64_t* rss_kib) {
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    PROCESS_MEMORY_COUNTERS counters = {};
    const bool ok = K32GetProcessMemoryInfo(
                        process, &counters, sizeof(counters)) != 0;
    CloseHandle(process);
    if (!ok) {
        return false;
    }
    *rss_kib = counters.WorkingSetSize / 1024ULL;
    return true;
}

// PSS (proportional set size) has no Windows equivalent; the caller
// falls back to RSS so the series never disappears.
bool read_self_pss_kib(std::uint64_t* pss_kib) {
    (void)pss_kib;
    return false;
}

// CPU time of a process from GetProcessTimes (kernel + user time), in
// 100 ns units; converted to whole seconds.
bool read_cpu_seconds(pid_t pid, std::uint64_t* cpu_seconds) {
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool ok = GetProcessTimes(
                        process, &creation, &exit, &kernel, &user) != 0;
    CloseHandle(process);
    if (!ok) {
        return false;
    }
    const auto to_uint64 = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
               static_cast<std::uint64_t>(value.dwLowDateTime);
    };
    *cpu_seconds =
        (to_uint64(kernel) + to_uint64(user)) / 10000000ULL;
    return true;
}

#else

constexpr unsigned long kPageSizeDivisor = 1024UL;

// Reads the RSS resident-set size of a process from /proc/<pid>/statm
// (field 2, in pages). Returns false when the process is gone or the
// read fails; the caller renders a zero series, never a fabrication.
bool read_rss_kib(pid_t pid, std::uint64_t* rss_kib) {
    char path[64];
    const int written =
        std::snprintf(path, sizeof(path), "/proc/%ld/statm",
                      static_cast<long>(pid));
    if (written <= 0 ||
        static_cast<std::size_t>(written) >= sizeof(path)) {
        return false;
    }
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    char line[256] = {};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) {
        return false;
    }
    unsigned long size_pages = 0;
    unsigned long rss_pages = 0;
    if (std::sscanf(line, "%lu %lu", &size_pages, &rss_pages) != 2) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return false;
    }
    const unsigned long page_kib =
        static_cast<unsigned long>(page_size) / kPageSizeDivisor;
    *rss_kib = rss_pages * page_kib;
    return true;
}

// Reads the PSS proportional-set size of the Host itself from
// /proc/self/smaps_rollup (the first "Pss:" line, in KiB). Returns false
// when unavailable (non-Linux, no smaps permission); the caller falls
// back to RSS so the series never disappears.
bool read_self_pss_kib(std::uint64_t* pss_kib) {
    FILE* file = std::fopen("/proc/self/smaps_rollup", "r");
    if (file == nullptr) {
        return false;
    }
    char line[256] = {};
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::sscanf(line, "Pss: %llu kB",
                        reinterpret_cast<unsigned long long*>(pss_kib)) ==
            1) {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

// CPU time of a process from /proc/<pid>/stat fields 14 and 15
// (utime, stime), in clock ticks; converted to whole seconds.
bool read_cpu_seconds(pid_t pid, std::uint64_t* cpu_seconds) {
    char path[64];
    const int written =
        std::snprintf(path, sizeof(path), "/proc/%ld/stat",
                      static_cast<long>(pid));
    if (written <= 0 ||
        static_cast<std::size_t>(written) >= sizeof(path)) {
        return false;
    }
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    // The comm field may contain spaces and parens; skip past the last
    // ')' to reach the fields that follow it.
    char line[1024] = {};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) {
        return false;
    }
    const char* state_start = std::strrchr(line, ')');
    if (state_start == nullptr || state_start[1] == '\0') {
        return false;
    }
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    // Fields after comm: 3=state 4=ppid 5=pgrp 6=session 7=tty 8=tpgid
    // 9=flags 10=minflt 11=cminflt 12=majflt 13=cmajflt 14=utime 15=stime.
    unsigned long long dummy[11] = {};
    if (std::sscanf(state_start + 1, " %llu %llu %llu %llu %llu %llu "
                                     "%llu %llu %llu %llu %llu %llu %llu",
                    &dummy[0], &dummy[1], &dummy[2], &dummy[3],
                    &dummy[4], &dummy[5], &dummy[6], &dummy[7], &dummy[8],
                    &dummy[9], &dummy[10], &utime, &stime) < 13) {
        return false;
    }
    const long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) {
        return false;
    }
    *cpu_seconds = (utime + stime) /
                   static_cast<unsigned long long>(clk_tck);
    return true;
}
#endif  // !defined(_WIN32)

}  // namespace

MetricsRegistry::ProcessSnapshotProvider default_process_snapshot_provider(
    const std::atomic<pid_t>* worker_pid) {
    return [worker_pid]() -> ProcessMetricsSnapshot {
        ProcessMetricsSnapshot snapshot;
        // Host own footprint.
        std::uint64_t rss_kib = 0;
#if defined(_WIN32)
        const pid_t host_pid = static_cast<pid_t>(capsid::win32::getpid());
#else
        const pid_t host_pid = static_cast<pid_t>(::getpid());
#endif
        if (read_rss_kib(host_pid, &rss_kib)) {
            snapshot.rss_bytes = rss_kib * 1024ULL;
            snapshot.pss_bytes = snapshot.rss_bytes;
        }
        std::uint64_t pss_kib = 0;
        if (read_self_pss_kib(&pss_kib)) {
            snapshot.pss_bytes = pss_kib * 1024ULL;
        }
        read_cpu_seconds(host_pid, &snapshot.cpu_seconds_total);
        // The currently active worker, when the Host has published one.
        if (worker_pid != nullptr) {
            const pid_t pid = worker_pid->load();
            if (pid > 0) {
                snapshot.worker_pid = static_cast<std::uint64_t>(pid);
                std::uint64_t worker_rss_kib = 0;
                if (read_rss_kib(pid, &worker_rss_kib)) {
                    snapshot.worker_rss_bytes = worker_rss_kib * 1024ULL;
                }
            }
        }
        return snapshot;
    };
}

}  // namespace capsid::host
