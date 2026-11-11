#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0602
#endif
// Complete-pool scheduling tests. Only this executable replaces allocation;
// production code has no scheduling hook. A calibrated transport allocation
// pauses only when its real call stack includes the adoption implementation.
#include "host/static_pool_server.h"
#include "win32_compat.h"

#if defined(_WIN32)
#include <dbghelp.h>
#else
#include <dlfcn.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define CAPSID_TEST_HAS_EXECINFO 1
#else
#include <unwind.h>
#endif
#if defined(CAPSID_TEST_STATIC_SYMBOLS)
#include <cerrno>
#include <link.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_probe {
struct Hit {
    std::array<char, 1024> symbol{};
    void *address = nullptr;
};
#if defined(_WIN32) || defined(CAPSID_TEST_STATIC_SYMBOLS)
struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    Hit hit;
};
// Resolve once, before any Worker or allocation pause exists. Symbol loading
// must not become part of the bounded scheduling window on the accept thread.
std::vector<Range> adoption_ranges;
#endif
#if defined(_WIN32)
BOOL CALLBACK remember_adoption(SYMBOL_INFO *info, ULONG, void *) {
    if ((std::strcmp(info->Name, "capsid::host::Impl::adopt_socket") == 0 ||
         std::strcmp(info->Name, "capsid::host::Impl::try_adopt_socket") == 0 ||
         std::strcmp(info->Name, "capsid::host::SingleWorkerServer::adopt_socket") == 0 ||
         std::strcmp(info->Name, "capsid::host::SingleWorkerServer::try_adopt_socket") == 0) &&
        info->Size > 0) {
        Hit hit;
        std::snprintf(hit.symbol.data(), hit.symbol.size(), "%s", info->Name);
        adoption_ranges.push_back({info->Address, info->Address + info->Size, hit});
    }
    return TRUE;
}
#endif
#if !defined(_WIN32) && !defined(CAPSID_TEST_HAS_EXECINFO)
struct StackCapture {
    void **frames;
    std::size_t capacity;
    std::size_t count = 0;
};
_Unwind_Reason_Code capture_frame(_Unwind_Context *context, void *data) {
    auto &capture = *static_cast<StackCapture *>(data);
    if (capture.count == capture.capacity)
        return _URC_END_OF_STACK;
    const auto address = _Unwind_GetIP(context);
    if (address != 0)
        capture.frames[capture.count++] = reinterpret_cast<void *>(address);
    return _URC_NO_REASON;
}
#endif
std::mutex symbols_mutex;
bool adoption_stack(Hit &hit) {
    void *frames[48] = {};
#if defined(_WIN32)

#if defined(_M_X64) || defined(__x86_64__)
    // The default capture returned zero for observed allocation stacks.
    // Walk only to the actual adoption call, using x64 unwind metadata.
    // Use the current thread's actual limits. VirtualQuery reports one
    // homogeneous page region, which is not the full stack reservation.
    ULONG_PTR stack_low = 0, stack_high = 0;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    CONTEXT context{};
    RtlCaptureContext(&context);
    if (stack_high <= stack_low || stack_high - stack_low < sizeof(DWORD64))
        return false;
    unsigned count = 0;
    while (count < 48 && context.Rip && context.Rsp >= stack_low &&
           context.Rsp <= stack_high - sizeof(DWORD64)) {
        frames[count++] = reinterpret_cast<void *>(context.Rip);
        bool found = false;
        for (const auto &range : adoption_ranges)
            if (context.Rip >= range.begin && context.Rip < range.end) {
                found = true;
                break;
            }
        if (found) break;
        const auto previous_stack = context.Rsp;
        DWORD64 image_base = 0;
        const auto function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function) {
            void *handler = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function,
                            &context, &handler, &establisher, nullptr);
        } else {
            // A leaf function has no unwind entry; emulate its return.
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (context.Rsp <= previous_stack) break;
    }
#else
    const auto count = CaptureStackBackTrace(0, 48, frames, nullptr);
#endif

#elif defined(CAPSID_TEST_HAS_EXECINFO)
    const auto count = ::backtrace(frames, 48);
#else
    StackCapture capture{frames, 48};
    _Unwind_Backtrace(capture_frame, &capture);
    const auto count = capture.count;
#endif
    std::lock_guard<std::mutex> lock(symbols_mutex);
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        const char *name = nullptr;
#if defined(_WIN32) || defined(CAPSID_TEST_STATIC_SYMBOLS)
        const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        for (const auto &range : adoption_ranges)
            if (address >= range.begin && address < range.end) {
                hit = range.hit;
                hit.address = frames[i];
                return true;
            }
#else
        Dl_info info{};
        if (::dladdr(frames[i], &info))
            name = info.dli_sname;
#endif
        if (name && std::strstr(name, "capsid") && std::strstr(name, "adopt_socket")) {
            std::snprintf(hit.symbol.data(), hit.symbol.size(), "%s", name);
            hit.address = frames[i];
            return true;
        }
    }
    return false;
}

class Pause {
  public:
    const std::thread::id main = std::this_thread::get_id();
    std::size_t expected = 0;
    void enter(const Hit &hit, std::size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (entered_)
            return;
        hit_ = hit;
        bytes_ = bytes;
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(6), [this] { return entered_; });
    }
    Hit hit() {
        std::lock_guard<std::mutex> lock(mutex_);
        return hit_;
    }
    std::size_t bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }
    void release() noexcept;

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    Hit hit_;
    std::size_t bytes_ = 0;
    bool entered_ = false;
    bool released_ = false;
};
std::atomic<Pause *> armed{nullptr};
thread_local std::size_t *calibration = nullptr;
thread_local bool inspecting = false;
void Pause::release() noexcept {
    armed.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
    }
    condition_.notify_all();
}
} // namespace allocation_probe

void *operator new(std::size_t bytes) {
    if (auto *record = std::exchange(allocation_probe::calibration, nullptr))
        *record = bytes;
    if (!allocation_probe::inspecting) {
        if (auto *pause = allocation_probe::armed.load(std::memory_order_acquire);
            pause && bytes == pause->expected && std::this_thread::get_id() != pause->main) {
            allocation_probe::inspecting = true;
            allocation_probe::Hit hit;
            if (allocation_probe::adoption_stack(hit))
                pause->enter(hit, bytes);
            allocation_probe::inspecting = false;
        }
    }
    if (void *memory = std::malloc(bytes ? bytes : 1))
        return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
    return std::malloc(bytes ? bytes : 1);
}
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept {
    return std::malloc(bytes ? bytes : 1);
}
#if defined(__GNUC__)
#define CAPSID_TEST_ALLOCATION_BOUNDARY __attribute__((noinline))
#else
#define CAPSID_TEST_ALLOCATION_BOUNDARY
#endif
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void *memory) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void *memory) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void *memory,
                                                     const std::nothrow_t &) noexcept {
    std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void *memory,
                                                       const std::nothrow_t &) noexcept {
    std::free(memory);
}
#undef CAPSID_TEST_ALLOCATION_BOUNDARY

namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string quote(const std::string &value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"')
            result += '\\';
        result += c >= 32 ? static_cast<char>(c) : '?';
    }
    return result + '"';
}
void event(const char *stage, const std::string &extra = {}) {
    std::cout << "{\"stage\":" << quote(stage) << extra << "}" << std::endl;
}
struct DriverClosed {};
std::string command() {
    std::string line;
    if (!std::getline(std::cin, line))
        throw DriverClosed{};
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}
struct ReadyPair {
    int fd[2] = {-1, -1};
    ReadyPair() { require(capsid::win32::create_socket_pair(fd), "create private READY pair"); }
    ~ReadyPair() {
        close(fd[0]);
        close(fd[1]);
    }
};
std::uint16_t read_port(int fd) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (line.find('\n') == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline, "READY deadline");
        capsid_pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "poll READY");
        if (!polled)
            continue;
        char bytes[512];
        const auto count = capsid::win32::read_fd(fd, bytes, sizeof(bytes));
        require(count > 0, "read READY");
        line.append(bytes, static_cast<std::size_t>(count));
    }
    const auto position = line.find("\"port\":");
    require(position != std::string::npos, "READY port field");
    const auto port = std::stoul(line.substr(position + 7));
    require(port > 0 && port <= 65535, "READY port range");
    return static_cast<std::uint16_t>(port);
}
void await_active(capsid::host::StaticPoolServer &pool, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (pool.active_workers() != expected && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(pool.active_workers() == expected, "actual Worker availability barrier");
}
void prepare_symbols() {
#if defined(_WIN32)
    char executable[32768];
    const auto length =
        GetModuleFileNameA(nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    require(length > 0 && length < sizeof(executable), "fixture executable path");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    const std::string path(executable);
    const auto directory = path.substr(0, path.find_last_of("\\/"));
    require(SymInitialize(GetCurrentProcess(), directory.c_str(), FALSE) != FALSE,
            "initialize local symbols");
    // Load only this executable and its own PDB. No symbol server is needed.
    require(SymLoadModuleEx(GetCurrentProcess(), nullptr, executable, nullptr,
                            reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr)), 0, nullptr,
                            0) != 0,
            "load fixture PDB");
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024] = {};
    auto *info = reinterpret_cast<SYMBOL_INFO *>(storage);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 1023;
    DWORD64 displacement = 0;
    require(SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(&prepare_symbols),
                        &displacement, info) &&
                std::strstr(info->Name, "prepare_symbols"),
            "fixture private symbols must resolve before any Worker starts");
    require(SymEnumSymbols(GetCurrentProcess(),
                           reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr)), "*adopt_socket*",
                           allocation_probe::remember_adoption, nullptr) &&
                !allocation_probe::adoption_ranges.empty(),
            "resolve actual adoption function ranges");
    require(SymCleanup(GetCurrentProcess()) != FALSE, "release startup symbol reader");
#elif defined(CAPSID_TEST_STATIC_SYMBOLS)
    struct MainImage {
        const void *headers;
        std::uintptr_t load_bias = 0;
        bool found = false;
    } image{reinterpret_cast<const void *>(::getauxval(AT_PHDR))};
    dl_iterate_phdr([](dl_phdr_info *info, std::size_t, void *data) {
        auto &image = *static_cast<MainImage *>(data);
        if (info->dlpi_phdr != image.headers)
            return 0;
        image.load_bias = info->dlpi_addr;
        image.found = true;
        return 1;
    }, &image);
    require(image.found, "locate the static fixture ELF load address");
    int descriptors[2];
    require(::pipe(descriptors) == 0, "create local symbol reader pipe");
    const std::string executable = "/proc/" + std::to_string(::getpid()) + "/exe";
    const auto child = ::fork();
    if (child == 0) {
        ::close(descriptors[0]);
        if (::dup2(descriptors[1], STDOUT_FILENO) < 0)
            ::_exit(126);
        ::close(descriptors[1]);
        ::execl(CAPSID_TEST_NM_PATH, CAPSID_TEST_NM_PATH, "--defined-only", "--print-size",
                "--format=posix", executable.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(descriptors[1]);
    if (child < 0) {
        ::close(descriptors[0]);
        require(false, "start local symbol reader");
    }
    FILE *symbols = ::fdopen(descriptors[0], "r");
    if (symbols == nullptr)
        ::close(descriptors[0]);
    std::array<char, 8192> line{};
    while (symbols != nullptr && std::fgets(line.data(), line.size(), symbols) != nullptr) {
        std::istringstream input(line.data());
        std::string name;
        char type = 0;
        std::uint64_t address = 0, size = 0;
        if (!(input >> name >> type >> std::hex >> address >> size) || size == 0 ||
            name.find("capsid") == std::string::npos || name.find("adopt_socket") == std::string::npos)
            continue;
        allocation_probe::Hit hit;
        std::snprintf(hit.symbol.data(), hit.symbol.size(), "%s", name.c_str());
        allocation_probe::adoption_ranges.push_back({image.load_bias + address, image.load_bias + address + size, hit});
    }
    const bool reader_ok = symbols != nullptr && std::ferror(symbols) == 0;
    if (symbols != nullptr)
        std::fclose(symbols);
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(reader_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                !allocation_probe::adoption_ranges.empty(),
            "resolve actual adoption function ranges from the static fixture");
#endif
}
void identify_darwin(const std::string &line) {
#if defined(__APPLE__)
    // XNU sysctl.h kinfo_proc and sys/proc.h p_starttime provide microseconds;
    // ps lstart has only seconds and cannot order these real child processes.
    std::istringstream input(line.substr(14));
    std::string values;
    for (int i = 0; i < 3; ++i) {
        int pid = 0;
        require(static_cast<bool>(input >> pid) && pid > 0, "three child PIDs");
        int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
        struct kinfo_proc info{};
        std::size_t length = sizeof(info);
        require(::sysctl(mib, 4, &info, &length, nullptr, 0) == 0 && length == sizeof(info),
                "child kernel identity");
        require(info.kp_eproc.e_ppid == ::getpid() && info.kp_proc.p_pid == pid,
                "actual direct child");
        const auto &time = info.kp_proc.p_starttime;
        const auto created = static_cast<std::uint64_t>(time.tv_sec) * 1000000U +
                             static_cast<std::uint64_t>(time.tv_usec);
        if (i)
            values += ',';
        values += "{\"pid\":" + std::to_string(pid) +
                  ",\"created\":" + quote(std::to_string(created)) + "}";
    }
    event("worker-times", ",\"workers\":[" + values + "]");
#else
    (void)line;
    require(false, "kernel child query is Darwin-only");
#endif
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 3, "expected actual Worker and scenario");
        const std::string scenario = argv[2];
        require(scenario == "reselect-worker-exit" || scenario == "reselect-last-workers-exit" ||
                    scenario == "pre-post-stop" || scenario == "pre-post-drain" ||
                    scenario == "active-drain",
                "known adoption scenario");
        prepare_symbols();
        allocation_probe::Pause pause;
        boost::asio::io_context calibration_io;
        allocation_probe::calibration = &pause.expected;
        const auto calibration = std::make_shared<boost::asio::ip::tcp::socket>(calibration_io);
        allocation_probe::calibration = nullptr;
        require(pause.expected > 0, "calibrate real socket transport allocation");
        ReadyPair ready;
        capsid::host::StaticPoolServerOptions options;
        options.workers = 3;
        auto &worker = options.worker_options;
        worker.worker_path = argv[1];
        worker.source_bundle_path = "static-pool-adoption-inline";
        worker.source_name = "file://orders/v1/bundle.mjs";
        worker.application = "orders";
        worker.listen_address = "127.0.0.1";
        worker.listen_port = 0;
        worker.public_scheme = "http";
        worker.public_authority = "public.example";
        worker.request_timeout_ms = 6000;
        worker.initial_stream_window = 64U * 1024U;
        worker.strict_sandbox = false;
        worker.install_process_signals = false;
        worker.ready_fd = ready.fd[1];
        const std::string source = R"js(
// Each READY finishes module evaluation before the next Worker is spawned.
// A short monotonic interval makes Linux's /proc creation ticks distinct.
const initialized = performance.now() + 20;
while (performance.now() < initialized) {}
const instance = crypto.randomUUID();
export default {fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/ws')) {
        const pair = new WebSocketPair(); pair[1].accept(); pair[1].binaryType = 'arraybuffer';
        pair[1].onmessage = event => pair[1].send(event.data);
        return new Response(null, {status: 101, webSocket: pair[0]});
    }
    return new Response(JSON.stringify({challenge: url.searchParams.get('challenge'), instance}));
}};
)js";
        capsid::host::StaticPoolServer pool(std::move(options));
        struct ReleaseOnExit {
            allocation_probe::Pause &pause;
            ~ReleaseOnExit() { pause.release(); }
        } release{pause};
        std::string error;
        require(pool.start(std::vector<std::uint8_t>(source.begin(), source.end()), &error),
                "start actual pool: " + error);
        require(pool.active_workers() == 3, "three Workers at READY");
        event("pool-ready", ",\"active\":3,\"port\":" + std::to_string(read_port(ready.fd[0])));
        bool completed = false;
        try {
            auto first = command();
            if (first.starts_with("created-times ")) {
                identify_darwin(first);
                first = command();
            }
            require(first == "await-first-exit", "expected first exit barrier");
            await_active(pool, 2);
            event("first-worker-exited", ",\"active\":2");
            require(command() == "arm", "expected probe arm");
            allocation_probe::armed.store(&pause, std::memory_order_release);
            event("probe-armed", ",\"expectedAllocationBytes\":" + std::to_string(pause.expected));
            require(pause.wait(), "real adoption transport allocation must pause");
            require(pause.bytes() == pause.expected && pool.active_workers() == 2,
                    "pause after live target preflight");
            const auto hit = pause.hit();
            std::ostringstream address;
            address << hit.address;
            event("adoption-paused",
                  ",\"active\":2,\"allocationBytes\":" + std::to_string(pause.bytes()) +
                      ",\"expectedAllocationBytes\":" + std::to_string(pause.expected) +
                      ",\"symbol\":" + quote(hit.symbol.data()) +
                      ",\"address\":" + quote(address.str()));
            const auto action = command();
            std::size_t remaining = 0;
            if (scenario.starts_with("reselect-")) {
                require(action == "worker-exit", "expected selected Worker exit");
                remaining = scenario == "reselect-worker-exit" ? 1 : 0;
            } else if (scenario == "pre-post-stop") {
                require(action == "stop", "expected pool stop");
                pool.request_stop();
            } else {
                require(action == "drain", "expected pool drain");
                pool.begin_drain(15000);
                remaining = scenario == "active-drain" ? 2 : 0;
            }
            await_active(pool, remaining);
            event("transition-observed", ",\"active\":" + std::to_string(remaining));
            require(command() == "release", "expected probe release");
            pause.release();
            event("probe-released");
            require(command() == "finish", "expected completed network assertions");
            completed = true;
        } catch (const DriverClosed &) {
            // The driver may have a primary network assertion failure. Still
            // release the pause and reap every owned process normally.
        }
        pause.release();
        const bool drain = scenario == "pre-post-drain" || scenario == "active-drain";
        if (!completed || !drain)
            pool.request_stop();
        require(pool.wait(&error), "join actual pool: " + error);
        require(pool.active_workers() == 0, "zero capacity after wait");
        const auto metrics = pool.drain_metrics();
        if (completed && drain)
            // Shard 0 exited before begin_drain and has no reactor left to
            // finish a drain. The aggregate AND therefore remains false;
            // wait() above still joins the remaining shards without stop().
            require(metrics.draining && !metrics.finished && metrics.forced_cancellations == 0,
                    "remaining shards drain naturally after an earlier Worker exit");
        event("stopped",
              ",\"active\":0,\"draining\":" + std::string(metrics.draining ? "true" : "false") +
                  ",\"finished\":" + std::string(metrics.finished ? "true" : "false") +
                  ",\"forcedCancellations\":" + std::to_string(metrics.forced_cancellations));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pool adoption fixture: " << error.what() << std::endl;
        return 1;
    }
}
