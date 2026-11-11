#include "host/parent_stop_monitor.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdint>
#endif
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
unsigned checks = 0;
bool check(bool condition, const char* message) {
    ++checks;
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

struct CloseFile {
    void operator()(std::FILE* file) const {
        if (file) std::fclose(file);
    }
};

struct Pipe {
    int descriptors[2] = {-1, -1};
    bool open() {
#if defined(_WIN32)
        return _pipe(descriptors, 64, _O_BINARY) == 0;
#else
        return pipe(descriptors) == 0;
#endif
    }
    bool write_marker() const {
#if defined(_WIN32)
        return _write(descriptors[1], "x", 1) == 1;
#else
        return write(descriptors[1], "x", 1) == 1;
#endif
    }
    bool read_marker() const {
        char value = 0;
#if defined(_WIN32)
        const auto count = _read(descriptors[0], &value, 1);
#else
        const auto count = read(descriptors[0], &value, 1);
#endif
        return count == 1 && value == 'x';
    }
    ~Pipe() {
        for (const int fd : descriptors) {
            if (fd < 0) continue;
#if defined(_WIN32)
            _close(fd);
#else
            close(fd);
#endif
        }
    }
};

#if defined(_MSC_VER) && defined(_DEBUG)
std::atomic<unsigned> reports{0}, invalid_parameters{0};
int observe_report(int type, char*, int* value) {
    if (type != _CRT_ASSERT && type != _CRT_WARN) return 0;
    reports.fetch_add(1);
    if (value) *value = 0;
    return 1;
}
int observe_report_wide(int type, wchar_t*, int* value) {
    return observe_report(type, nullptr, value);
}
void observe_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                               std::uintptr_t) {
    invalid_parameters.fetch_add(1);
}
struct ReportObserver {
    _invalid_parameter_handler previous = nullptr;
    bool narrow = false, wide = false;
    bool install() {
        previous = _set_thread_local_invalid_parameter_handler(observe_invalid_parameter);
        narrow = _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, observe_report) >= 0;
        wide = _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, observe_report_wide) >= 0;
        return narrow && wide;
    }
    ~ReportObserver() {
        if (wide) _CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, observe_report_wide);
        if (narrow) _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, observe_report);
        _set_thread_local_invalid_parameter_handler(previous);
    }
};
#endif

bool basic_descriptors() {
    std::string error;
    Pipe ownership;
    if (!check(ownership.open(), "create ownership pipe")) return false;
    auto monitor = capsid::host::ParentStopMonitor::create(ownership.descriptors[0], &error);
    if (!check(monitor != nullptr, "accept readable pipe")) return false;
    monitor.reset();
    if (!check(ownership.write_marker() && ownership.read_marker(),
               "destroy preserves the caller's original pipe descriptors")) return false;
    if (!check(!capsid::host::ParentStopMonitor::create(ownership.descriptors[1], &error) && !error.empty(),
               "reject a pipe's write-only descriptor")) return false;

#if defined(_MSC_VER)
    std::FILE* raw_file = nullptr;
    if (tmpfile_s(&raw_file) != 0) raw_file = nullptr;
#else
    std::FILE* raw_file = std::tmpfile();
#endif
    auto file = std::unique_ptr<std::FILE, CloseFile>(raw_file);
    if (!check(file != nullptr, "create owned regular file")) return false;
#if defined(_WIN32)
    const int fd = _fileno(file.get());
#else
    const int fd = fileno(file.get());
#endif
    if (!check(!capsid::host::ParentStopMonitor::create(fd, &error) && !error.empty(),
               "reject regular files")) return false;
    if (!check(!capsid::host::ParentStopMonitor::create(INT_MAX, &error) &&
                   error.find("readable pipe") != std::string::npos,
               "reject invalid descriptor with a normal diagnostic")) return false;

    Pipe watched;
    if (!check(watched.open(), "create watched pipe")) return false;
    monitor = capsid::host::ParentStopMonitor::create(watched.descriptors[0], &error);
    if (!check(monitor != nullptr, "create actual watcher")) return false;
    std::atomic<unsigned> callbacks{0};
    if (!check(monitor->start([&] { callbacks.fetch_add(1); }, &error), "start actual watcher")) return false;
    if (!check(watched.write_marker(), "write real stop byte")) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callbacks.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    monitor.reset();
    if (!check(callbacks.load() == 1, "stop callback runs once and joins before destruction")) return false;
#if !defined(_WIN32)
    int sockets[2] = {-1, -1};
    if (!check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create owned local socket pair")) return false;
    monitor = capsid::host::ParentStopMonitor::create(sockets[0], &error);
    const bool accepted = monitor != nullptr;
    monitor.reset();
    close(sockets[0]); close(sockets[1]);
    if (!check(accepted, "accept readable local socket on Unix")) return false;
#endif
    return true;
}
}  // namespace

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    // Observe expected CRT diagnostics without a dialog. An unscoped product
    // implementation reports the invalid input here and fails the check below.
    ReportObserver observer;
    if (!check(observer.install(), "install local CRT observation controls")) return 1;
#endif
    if (!basic_descriptors()) return 1;
#if defined(_MSC_VER) && defined(_DEBUG)
    if (!check(reports.load() == 0 && invalid_parameters.load() == 0,
               "expected descriptor rejection stays inside its own CRT probe")) return 1;
    std::atomic<unsigned> failures{0};
    const auto probe = [&] {
        for (unsigned i = 0; i < 100; ++i) {
            std::string error;
            if (capsid::host::ParentStopMonitor::create(INT_MAX, &error) ||
                error.find("readable pipe") == std::string::npos) failures.fetch_add(1);
            // Concurrent warnings avoid the CRT's own simultaneous-assertion
            // recursion path while checking reports on the other thread.
            _CrtDbgReport(_CRT_WARN, "parent-stop-control", 1, nullptr, "unrelated warning");
        }
    };
    std::thread first(probe), second(probe);
    first.join(); second.join();
    if (!check(failures.load() == 0 && reports.load() == 200,
               "concurrent invalid probes preserve unrelated warnings")) return 1;
    // Real CRT rejection outside the product probe verifies restoration of
    // both the report hooks and this thread's invalid-parameter handler.
    if (!check(_get_osfhandle(INT_MAX) == -1, "outside control returns EBADF")) return 1;
    if (!check(reports.load() == 201 && invalid_parameters.load() == 1,
               "outside assertion and previous invalid-parameter handler remain observable")) return 1;
#endif
    std::printf("PASS: parent stop monitor (%u checks)\n", checks);
    return 0;
}
