// Spawn failure recovery (v0.3.0 remediation T02).
//
// Contract: docs/v0.3.0-implementation-design.md §2.3 — a rejected or
// failing spawn leaves nothing behind: no child process (fake-child
// marker stays absent, no zombie or live descendants of this test), no
// leaked descriptors, no accumulated residue across repeated failures.
// Validation of protocol options happens before any OS side effect
// (T01); this file locks that in from the process side.
#include "capsid/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char *kMarkerPath = "/tmp/capsid-spawn-recovery-marker";

void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

void require_result(capsid_result result, capsid_result expected,
                    const char *what) {
    if (result != expected) {
        fail(std::string(what) + ": expected " +
             capsid_result_string(expected) + ", got " +
             capsid_result_string(result));
    }
}

// Descriptors currently open in this process.
size_t fd_count() {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        fail("opendir /proc/self/fd");
    }
    size_t count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);
    // readdir needs a descriptor itself; subtract the two entries "." /
    // ".." plus the directory fd.
    return count > 3 ? count - 3 : 0;
}

// Live or zombie descendants of this process (scoped to children this
// test owns; no global process scans).
size_t own_children() {
    DIR *dir = opendir("/proc");
    if (!dir) {
        return 0;
    }
    size_t count = 0;
    struct dirent *entry = nullptr;
    const pid_t me = getpid();
    while ((entry = readdir(dir)) != nullptr) {
        char *end = nullptr;
        const long value = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || value <= 0) {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%ld/stat", value);
        FILE *stat = std::fopen(path, "r");
        if (!stat) {
            continue;
        }
        char buffer[512];
        const char *line = std::fgets(buffer, sizeof(buffer), stat);
        std::fclose(stat);
        if (!line) {
            continue;
        }
        // Fields: pid (comm) state ppid ...
        char *close_paren = std::strrchr(buffer, ')');
        if (!close_paren) {
            continue;
        }
        int state_c = 0;
        long ppid = 0;
        if (std::sscanf(close_paren + 2, "%c %ld", &state_c, &ppid) != 2) {
            continue;
        }
        if (ppid == me) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

void write_marker_child(const char *path) {
    FILE *script = std::fopen(path, "w");
    if (!script) {
        fail("marker script open");
    }
    std::fputs("#!/bin/sh\ntouch ", script);
    std::fputs(kMarkerPath, script);
    std::fputs("\nsleep 30\n", script);
    std::fclose(script);
    chmod(path, 0755);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <capsid-worker>\n", argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];
    std::string marker_script = "/tmp/capsid-spawn-recovery-child.sh";
    write_marker_child(marker_script.c_str());
    unlink(kMarkerPath);

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;

    capsid_protocol_options options;
    require_result(
        capsid_protocol_options_init(&options, sizeof(options)),
        CAPSID_OK, "options init");
    options.wire_version = 4;

    // 1. Rejected options never create a process: a marker-writing fake
    //    child behind the same config must not run (25 repeats to catch
    //    anything racy), and no descriptors or children accumulate.
    const size_t fds_before = fd_count();
    for (int i = 0; i < 25; ++i) {
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(0x1);
        require_result(
            capsid_worker_spawn_ex(&config, nullptr, &worker),
            CAPSID_OK, "null options must still spawn");
        // drain READY and destroy: proves the loop leaves no children
        capsid_worker_destroy(worker);
    }
    const size_t fds_after_valid = fd_count();
    if (fds_after_valid != fds_before) {
        fail("descriptor leak across valid spawn/destroy cycles: " +
             std::to_string(fds_before) + " -> " +
             std::to_string(fds_after_valid));
    }
    if (own_children() != 0) {
        fail("children remain after destroy");
    }

    // 2. Invalid options against the marker child: the child must never
    //    execute, nothing may accumulate.
    config.worker_path = marker_script.c_str();
    options.wire_version = 5;
    for (int i = 0; i < 25; ++i) {
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(0x1);
        require_result(
            capsid_worker_spawn_ex(&config, &options, &worker),
            CAPSID_INVALID_ARGUMENT, "invalid wire version");
        if (worker != nullptr) {
            fail("rejected spawn left a non-null out param");
        }
    }
    struct stat marker_stat;
    if (stat(kMarkerPath, &marker_stat) == 0) {
        fail("marker child executed despite rejected options");
    }
    if (own_children() != 0) {
        fail("children remain after rejected spawns");
    }
    if (fd_count() != fds_before) {
        fail("descriptor leak across rejected spawns");
    }

    // 3. Nonexistent worker path: posix_spawn itself fails; no residue.
    config.worker_path = "/nonexistent/capsid-worker-for-recovery-test";
    for (int i = 0; i < 5; ++i) {
        capsid_worker *worker = nullptr;
        const capsid_result result =
            capsid_worker_spawn(&config, &worker);
        if (result != CAPSID_CHILD_ERROR) {
            fail("nonexistent path must be CHILD_ERROR, got " +
                 std::string(capsid_result_string(result)));
        }
    }
    if (own_children() != 0 || fd_count() != fds_before) {
        fail("spawn failure left processes or descriptors behind");
    }

    unlink(kMarkerPath);
    unlink(marker_script.c_str());
    if (own_children() != 0) {
        fail("children appeared during the final checks");
    }
    std::printf("spawn failure recovery: PASS\n");
    return 0;
}

#elif defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

DWORD handle_count() {
    DWORD result = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &result) != FALSE,
            "GetProcessHandleCount failed");
    return result;
}

std::vector<DWORD> own_children() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "process snapshot failed");
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> children;
    BOOL more = Process32FirstW(snapshot, &entry);
    while (more) {
        if (entry.th32ParentProcessID == GetCurrentProcessId()) children.push_back(entry.th32ProcessID);
        more = Process32NextW(snapshot, &entry);
    }
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    require(error == ERROR_NO_MORE_FILES, "process enumeration failed");
    return children;
}

std::wstring executable_path() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(size != 0 && size < path.size(), "executable path unavailable");
    return std::wstring(path.data(), size);
}

std::string narrow(const std::wstring& value) {
    BOOL substituted = FALSE;
    const UINT codepage = GetACP();
    const DWORD flags = codepage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* used_default = codepage == CP_UTF8 ? nullptr : &substituted;
    const int count = WideCharToMultiByte(codepage, flags, value.c_str(), -1,
                                         nullptr, 0, nullptr, used_default);
    require(count > 0 && !substituted, "worker path is not representable for CreateProcessA");
    std::vector<char> bytes(static_cast<size_t>(count));
    substituted = FALSE;
    require(WideCharToMultiByte(codepage, flags, value.c_str(), -1,
            bytes.data(), count, nullptr, used_default) == count && !substituted, "worker path conversion failed");
    return bytes.data();
}

class TemporaryFixture {
public:
    std::wstring directory, helper, marker;
    std::vector<std::wstring> files;
    explicit TemporaryFixture(const std::wstring& real_worker) {
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD count = GetTempPathW(MAX_PATH + 1, temp);
        require(count != 0 && count <= MAX_PATH, "temporary directory unavailable");
        directory = std::wstring(temp) + L"capsid-spawn-recovery-" + std::to_wstring(GetCurrentProcessId()) +
                    L"-" + std::to_wstring(GetTickCount64());
        require(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "cannot create owned temporary directory");
        helper = directory + L"\\marker-child.exe";
        marker = directory + L"\\marker";
        files.push_back(marker);
        files.push_back(helper);
        require(CopyFileW(executable_path().c_str(), helper.c_str(), TRUE) != FALSE, "cannot copy marker helper");
        // The production launcher supplies only SystemRoot. Place the same
        // deployed ASan/CRT DLLs beside the helper, so its positive witness
        // exercises that real launch environment, without depending on PATH.
        const auto slash = real_worker.find_last_of(L"\\/");
        require(slash != std::wstring::npos, "worker must have an absolute path");
        const auto worker_directory = real_worker.substr(0, slash);
        WIN32_FIND_DATAW found = {};
        HANDLE search = FindFirstFileW((worker_directory + L"\\*.dll").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                const auto target = directory + L"\\" + found.cFileName;
                const auto from = worker_directory + L"\\" + found.cFileName;
                if (!CopyFileW(from.c_str(), target.c_str(), TRUE)) {
                    FindClose(search);
                    throw std::runtime_error("cannot deploy helper runtime DLL");
                }
                files.push_back(target);
            } while (FindNextFileW(search, &found));
            FindClose(search);
        } else require(GetLastError() == ERROR_FILE_NOT_FOUND, "cannot enumerate helper runtime DLLs");
    }
    bool marker_exists() const { return GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES; }
    void clear_marker() {
        require(DeleteFileW(marker.c_str()) != FALSE, "cannot clear positive marker witness");
    }
    ~TemporaryFixture() {
        for (const auto& file : files) DeleteFileW(file.c_str());
        RemoveDirectoryW(directory.c_str());
    }
};

int marker_child(const std::wstring& executable) {
    const auto slash = executable.find_last_of(L"\\/");
    const auto marker = executable.substr(0, slash) + L"\\marker";
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 31;
    CloseHandle(file);
    Sleep(20);
    return 0;
}

void no_residue(DWORD before, const char* phase) {
    require(own_children().empty(), std::string(phase) + ": child remains after destroy/rejection");
    const DWORD after = handle_count();
    require(after == before, std::string(phase) + ": handle count changed " +
            std::to_string(before) + " -> " + std::to_string(after));
}

void validate_observers(TemporaryFixture& fixture) {
    const DWORD baseline = handle_count();
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(event != nullptr, "handle observer control creation failed");
    const DWORD with_event = handle_count();
    CloseHandle(event);
    require(with_event == baseline + 1 && handle_count() == baseline,
            "handle observer missed a real Win32 handle");
    DWORD previous = baseline;
    // The first direct Win32 process creation can initialize process-wide
    // system handles. Observe it before calling Capsid, then require two
    // further direct control launches to return the exact same baseline.
    // No offset or tolerance is applied to any product spawn below.
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        std::wstring command = L"\"" + fixture.helper + L"\"";
        require(CreateProcessW(fixture.helper.c_str(), &command[0], nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
                "process observer control creation failed");
        const auto children = own_children();
        const bool observed = std::find(children.begin(), children.end(), process.dwProcessId) != children.end();
        const bool resumed = ResumeThread(process.hThread) != static_cast<DWORD>(-1);
        const DWORD waited = resumed ? WaitForSingleObject(process.hProcess, 5000) : WAIT_FAILED;
        if (waited != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 32);
            WaitForSingleObject(process.hProcess, 5000);
        }
        DWORD exit_code = STILL_ACTIVE;
        const BOOL got_exit = GetExitCodeProcess(process.hProcess, &exit_code);
        const BOOL thread_closed = CloseHandle(process.hThread);
        const BOOL process_closed = CloseHandle(process.hProcess);
        require(thread_closed && process_closed, "native process control handles did not close");
        require(observed, "process observer missed a real owned suspended child");
        require(resumed && waited == WAIT_OBJECT_0 && got_exit && exit_code == 0 && fixture.marker_exists(),
                "native marker executable positive witness failed");
        fixture.clear_marker();
        require(own_children().empty(), "native process control child remains");
        const DWORD current = handle_count();
        std::printf("WIN32_OBSERVER iteration=%u before=%lu after=%lu\n", iteration,
                    static_cast<unsigned long>(previous), static_cast<unsigned long>(current));
        if (iteration != 0) require(current == previous, "native observer controls accumulate handles");
        previous = current;
    }
}

class WinsockControl {
public:
    WinsockControl() {
        const DWORD before = handle_count();
        WSADATA data = {};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "native Winsock control startup failed");
        std::printf("WIN32_NETWORK startup_before=%lu startup_after=%lu\n",
                    static_cast<unsigned long>(before), static_cast<unsigned long>(handle_count()));
    }
    ~WinsockControl() { WSACleanup(); }
};

void validate_network_controls() {
    DWORD previous = handle_count();
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET, "native network listener creation failed");
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int size = sizeof(address);
        bool ok = bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                  listen(listener, 1) == 0 && getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0;
        SOCKET sender = INVALID_SOCKET, receiver = INVALID_SOCKET;
        if (ok) {
            sender = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ok = sender != INVALID_SOCKET && connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        }
        if (ok) {
            receiver = accept(listener, nullptr, nullptr);
            ok = receiver != INVALID_SOCKET;
        }
        char byte = 0;
        if (ok) ok = send(sender, "x", 1, 0) == 1 && recv(receiver, &byte, 1, 0) == 1 && byte == 'x';
        if (receiver != INVALID_SOCKET) ok = closesocket(receiver) == 0 && ok;
        if (sender != INVALID_SOCKET) ok = closesocket(sender) == 0 && ok;
        ok = closesocket(listener) == 0 && ok;
        require(ok, "native loopback socket control failed");
        const DWORD current = handle_count();
        std::printf("WIN32_NETWORK iteration=%u before=%lu after=%lu\n", iteration,
                    static_cast<unsigned long>(previous), static_cast<unsigned long>(current));
        if (iteration != 0) require(current == previous, "native network control handles accumulate");
        previous = current;
    }
}

using Worker = std::unique_ptr<capsid_worker, void (*)(capsid_worker*)>;
void healthy_cycle(capsid_worker_config& config, const capsid_protocol_options* options, bool round_trip = false) {
    capsid_worker* raw = nullptr;
    const auto result = capsid_worker_spawn_ex(&config, options, &raw);
    Worker worker(raw, capsid_worker_destroy);
    require(result == CAPSID_OK && raw != nullptr, "valid spawn failed");
    const char bundle[] = "export default {fetch(){return new Response('healthy')}};";
    require(capsid_worker_load_bundle(raw, reinterpret_cast<const uint8_t*>(bundle), sizeof(bundle) - 1) == CAPSID_OK,
            "healthy bundle queue failed");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        const auto flush = capsid_worker_flush(raw);
        require(flush == CAPSID_OK || flush == CAPSID_WOULD_BLOCK, "healthy flush failed");
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const auto next = capsid_worker_next_event(raw, &event);
        if (next == CAPSID_OK && event.type == CAPSID_EVENT_READY) break;
        require(next == CAPSID_OK || next == CAPSID_WOULD_BLOCK, "healthy IPC failed before READY");
        require(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT, "healthy worker failed before READY");
        require(std::chrono::steady_clock::now() < deadline, "healthy READY deadline exceeded");
        Sleep(1);
    }
    if (round_trip) {
        require(capsid_worker_begin_request(raw, 1, "GET", "https://recovery.test/", nullptr, 0) == CAPSID_OK,
                "post-failure request could not begin");
        require(capsid_worker_end_request(raw, 1) == CAPSID_OK, "post-failure request could not end");
        unsigned heads = 0;
        std::string body;
        bool ended = false;
        while (!ended) {
            const auto flush = capsid_worker_flush(raw);
            require(flush == CAPSID_OK || flush == CAPSID_WOULD_BLOCK, "post-failure request flush failed");
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const auto next = capsid_worker_next_event(raw, &event);
            require(next == CAPSID_OK || next == CAPSID_WOULD_BLOCK, "post-failure response IPC failed");
            if (next == CAPSID_OK) {
                require(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT, "post-failure worker failed");
                if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                    require(event.request_id == 1 && event.status == 200 && ++heads == 1, "post-failure response head mismatch");
                } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                    require(event.request_id == 1 && heads == 1, "post-failure response body has no matching head");
                    body.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
                } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                    require(event.request_id == 1 && heads == 1 && body == "healthy", "post-failure response bytes mismatch");
                    ended = true;
                }
            }
            require(std::chrono::steady_clock::now() < deadline, "post-failure response deadline exceeded");
            if (!ended) Sleep(1);
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto executable = executable_path();
        if (executable.substr(executable.find_last_of(L"\\/") + 1) == L"marker-child.exe") return marker_child(executable);
        require(argc == 2, "usage: test-spawn-failure-recovery <capsid-worker>");
        const int length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
        require(length > 0, "worker path conversion failed");
        std::vector<wchar_t> path(static_cast<size_t>(length));
        require(MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, argv[1], -1, path.data(), length) == length,
                "worker path conversion failed");
        TemporaryFixture fixture(path.data());
        validate_observers(fixture);
        WinsockControl winsock;
        validate_network_controls();
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[1];
        const DWORD baseline = handle_count();
        for (int i = 0; i < 25; ++i) {
            healthy_cycle(config, nullptr);
            no_residue(baseline, "valid spawn/destroy");
        }
        const auto helper = narrow(fixture.helper);
        config.worker_path = helper.c_str();
        capsid_protocol_options options;
        require(capsid_protocol_options_init(&options, sizeof(options)) == CAPSID_OK, "protocol options init failed");
        options.wire_version = 4;
        {
            capsid_worker* raw = nullptr;
            const auto result = capsid_worker_spawn_ex(&config, &options, &raw);
            Worker worker(raw, capsid_worker_destroy);
            require(result == CAPSID_OK && raw != nullptr, "marker child could not start through actual C ABI");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!fixture.marker_exists()) {
                require(std::chrono::steady_clock::now() < deadline, "actual C ABI marker positive witness did not execute");
                Sleep(1);
            }
        }
        fixture.clear_marker();
        no_residue(baseline, "actual C ABI marker child");
        options.wire_version = 5;
        for (int i = 0; i < 25; ++i) {
            capsid_worker* worker = reinterpret_cast<capsid_worker*>(0x1);
            const auto result = capsid_worker_spawn_ex(&config, &options, &worker);
            require(result == CAPSID_INVALID_ARGUMENT && worker == nullptr, "invalid protocol options did not reject and clear output");
            require(!fixture.marker_exists(), "rejected options launched the real marker child");
            no_residue(baseline, "rejected spawn");
        }
        const auto missing = narrow(fixture.directory + L"\\does-not-exist.exe");
        config.worker_path = missing.c_str();
        for (int i = 0; i < 5; ++i) {
            capsid_worker* worker = reinterpret_cast<capsid_worker*>(0x1);
            const auto result = capsid_worker_spawn(&config, &worker);
            require(result == CAPSID_CHILD_ERROR && worker == nullptr, "nonexistent executable must fail without output worker");
            no_residue(baseline, "failed CreateProcess");
        }
        config.worker_path = argv[1];
        healthy_cycle(config, nullptr, true);
        no_residue(baseline, "post-failure healthy recovery");
        std::printf("spawn failure recovery (Windows): PASS valid=25 rejected=25 missing=5 recovered=1 handles=%lu\n",
                    static_cast<unsigned long>(baseline));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}

#else

int main(void) {
    std::fprintf(stderr, "SKIP: this recovery fixture requires Linux /proc; "
                         "native process/handle recovery coverage is pending\n");
    return 77;
}

#endif
