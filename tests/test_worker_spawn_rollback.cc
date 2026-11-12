// Exercise real public spawn failures after creating an owned native child.
// The client test object alone uses the forced-include call adapters. Kernel
// exit/reap witnesses and cleanup below always use the original OS APIs.
#include "capsid/runtime.h"
#include "worker_spawn_rollback_hooks.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#endif

namespace {
struct Fault {
    bool active = false, after_spawn = false, constructor = false;
    bool injected = false, nothrow_seen = false, killed = false;
    unsigned mode = 0, interruptions = 0, remaining = 0, wait_calls = 0;
    unsigned delay_ms = 0;
#if defined(_WIN32)
    HANDLE child = nullptr, witness = nullptr;
    bool asynchronous_applied = false, termination_ok = false;
#else
    pid_t child = -1;
#endif
} fault;
#if defined(_WIN32)
std::thread terminator;
#endif
}

#if defined(_MSC_VER)
#define ROLLBACK_NOINLINE __declspec(noinline)
#else
#define ROLLBACK_NOINLINE __attribute__((noinline))
#endif
ROLLBACK_NOINLINE void* operator new(std::size_t size) {
    if (fault.constructor) {
        fault.constructor = false;
        fault.injected = true;
        if (fault.mode == 2) throw std::runtime_error("controlled constructor failure");
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
ROLLBACK_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
ROLLBACK_NOINLINE void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* value = std::malloc(size ? size : 1);
    if (fault.after_spawn) {
        fault.after_spawn = false; fault.nothrow_seen = true;
        if (fault.mode == 0) { fault.injected = true; std::free(value); return nullptr; }
        fault.constructor = true;
    }
    return value;
}
ROLLBACK_NOINLINE void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
ROLLBACK_NOINLINE void operator delete(void* value) noexcept { std::free(value); }
ROLLBACK_NOINLINE void operator delete[](void* value) noexcept { std::free(value); }
ROLLBACK_NOINLINE void operator delete(void* value, std::size_t) noexcept { std::free(value); }
ROLLBACK_NOINLINE void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
ROLLBACK_NOINLINE void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
ROLLBACK_NOINLINE void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }

#if defined(_WIN32)
extern "C" BOOL WINAPI capsid_test_rollback_create(
    LPCSTR app, LPSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags,
    LPVOID environment, LPCSTR directory, LPSTARTUPINFOA startup,
    LPPROCESS_INFORMATION process) {
    const BOOL created = CreateProcessA(app, command, process_attributes,
        thread_attributes, inherit, flags, environment, directory, startup, process);
    if (created && fault.active) {
        fault.child = process->hProcess;
        if (!DuplicateHandle(GetCurrentProcess(), process->hProcess, GetCurrentProcess(),
                &fault.witness, SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
            TerminateProcess(process->hProcess, 1); WaitForSingleObject(process->hProcess, INFINITE);
            CloseHandle(process->hThread); CloseHandle(process->hProcess);
            return FALSE;
        }
        fault.after_spawn = true;
    }
    return created;
}
extern "C" BOOL WINAPI capsid_test_rollback_terminate(HANDLE child, UINT code) {
    if (!fault.active || child != fault.child) return TerminateProcess(child, code);
    fault.killed = true;
    if (!fault.delay_ms) {
        const BOOL result = TerminateProcess(child, code);
        fault.termination_ok = result != FALSE;
        return result;
    }
    HANDLE delayed = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), &delayed,
            SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) return FALSE;
    const unsigned delay = fault.delay_ms;
    try {
        // Model the documented asynchronous completion window. The wait and
        // pre-cleanup witness remain genuine kernel waits on a real process.
        terminator = std::thread([delayed, delay, code] {
            Sleep(delay);
            fault.termination_ok = TerminateProcess(delayed, code) != FALSE;
            CloseHandle(delayed);
        });
        fault.asynchronous_applied = true;
        return TRUE;
    } catch (...) {
        CloseHandle(delayed);
        return TerminateProcess(child, code);
    }
}
#else
extern "C" int capsid_test_rollback_spawn(pid_t* child, const char* file,
    const posix_spawn_file_actions_t* actions, const posix_spawnattr_t* attributes,
    char* const arguments[], char* const environment[]) {
    const int result = posix_spawn(child, file, actions, attributes, arguments, environment);
    if (!result && fault.active) { fault.child = *child; fault.after_spawn = true; }
    return result;
}
extern "C" int capsid_test_rollback_kill(pid_t child, int number) {
    const int result = kill(child, number);
    if (fault.active && child == fault.child && number == SIGKILL && !result) fault.killed = true;
    return result;
}
extern "C" pid_t capsid_test_rollback_wait(pid_t child, int* status, int options) {
    if (fault.active && fault.killed && child == fault.child && !options) {
        ++fault.wait_calls;
        if (fault.remaining) { --fault.remaining; ++fault.interruptions; errno = EINTR; return -1; }
    }
    return waitpid(child, status, options);
}
#endif

namespace {
bool healthy(const char* worker_path) {
    capsid_worker_config config; capsid_worker_config_init(&config);
    config.worker_path = worker_path; config.process_memory_limit = 0;
    capsid_worker* worker = nullptr;
    if (capsid_worker_spawn(&config, &worker) != CAPSID_OK || !worker) return false;
    static const char bundle[] = "export default {fetch(){return new Response('healthy')}}";
    bool passed = capsid_worker_load_bundle(worker, reinterpret_cast<const uint8_t*>(bundle), sizeof(bundle)-1) == CAPSID_OK;
    bool ready = false, ended = false; unsigned heads = 0; std::string body;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (passed && !ended && std::chrono::steady_clock::now() < deadline) {
        const auto flushed = capsid_worker_flush(worker);
        if (flushed != CAPSID_OK && flushed != CAPSID_WOULD_BLOCK) { passed = false; break; }
        capsid_event event{}; event.struct_size = sizeof(event);
        const auto result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_WOULD_BLOCK) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        if (result != CAPSID_OK) { passed = false; break; }
        if (event.type == CAPSID_EVENT_READY) {
            if (ready) { passed = false; break; }
            ready = true;
            passed = capsid_worker_begin_request(worker, 1, "GET", "https://rollback.test/", nullptr, 0) == CAPSID_OK &&
                capsid_worker_end_request(worker, 1) == CAPSID_OK;
        } else if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
            passed = ready && event.request_id == 1 && event.credit > 0;
        } else if (event.type == CAPSID_EVENT_RESPONSE_HEAD) passed = ready && event.request_id == 1 && event.status == 200 && ++heads == 1;
        else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            passed = ready && heads == 1 && event.request_id == 1;
            body.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
        } else if (event.type == CAPSID_EVENT_RESPONSE_END) ended = ready && heads == 1 && event.request_id == 1 && body == "healthy";
        else {
            std::fprintf(stderr, "FAIL: unexpected healthy request event type=%u request=%llu status=%u\n",
                static_cast<unsigned>(event.type), static_cast<unsigned long long>(event.request_id), event.status);
            passed = false;
        }
    }
    capsid_worker_destroy(worker);
    return passed && ended && body == "healthy";
}
unsigned descriptor_count() {
#if defined(_WIN32)
    DWORD value = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &value)) return 0;
    return value;
#else
    // Enumerate the complete native descriptor directory, excluding only
    // this query's own descriptor. Never consume any child exit here.
#if defined(__linux__)
    DIR* directory = opendir("/proc/self/fd");
#else
    DIR* directory = opendir("/dev/fd");
#endif
    if (!directory) return 0;
    unsigned count = 0;
    const int query_fd = dirfd(directory);
    while (dirent* entry = readdir(directory)) {
        char* end = nullptr;
        const long descriptor = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && !*end && descriptor >= 0 && descriptor != query_fd) ++count;
    }
    closedir(directory);
    return count;
#endif
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    if (!healthy(argv[1])) { std::fprintf(stderr,"FAIL: initial real healthy request\n"); return 2; }
    const unsigned before = descriptor_count(); if (!before) return 2;
    bool passed = true;
    for (unsigned mode = 0; mode < 3; ++mode) {
#if defined(_WIN32)
        const unsigned parameters[] = {0, 350};
#else
        const unsigned parameters[] = {0, 1, 3};
#endif
        for (unsigned parameter : parameters) {
            fault = Fault{}; fault.active = true; fault.mode = mode;
#if defined(_WIN32)
            fault.delay_ms = parameter;
#else
            fault.remaining = parameter;
#endif
            capsid_worker_config config; capsid_worker_config_init(&config);
            config.worker_path = argv[2]; config.process_memory_limit = 0;
            capsid_worker* worker = reinterpret_cast<capsid_worker*>(1);
            const auto result = capsid_worker_spawn(&config, &worker);
            fault.active = false; fault.after_spawn = false; fault.constructor = false;
            const bool error = result == (mode == 2 ? CAPSID_INTERNAL_ERROR : CAPSID_OUT_OF_MEMORY) &&
                worker == nullptr && capsid_last_error() && std::strlen(capsid_last_error()) > 0;
            bool completed = false, exact = false;
#if defined(_WIN32)
            completed = fault.witness && WaitForSingleObject(fault.witness, 0) == WAIT_OBJECT_0;
            if (terminator.joinable()) terminator.join();
            exact = fault.killed && fault.termination_ok && (!parameter || fault.asynchronous_applied);
            if (fault.witness) {
                if (WaitForSingleObject(fault.witness, 0) != WAIT_OBJECT_0) TerminateProcess(fault.witness, 1);
                const DWORD waited = WaitForSingleObject(fault.witness, 5000);
                DWORD status = STILL_ACTIVE;
                exact = exact && waited == WAIT_OBJECT_0 && GetExitCodeProcess(fault.witness, &status) && status == 1;
                CloseHandle(fault.witness); fault.witness = nullptr;
            }
#else
            if (fault.child > 0) {
                siginfo_t info{}; int waited;
                do { errno = 0; waited = waitid(P_PID, static_cast<id_t>(fault.child), &info, WEXITED | WNOHANG | WNOWAIT); }
                while (waited < 0 && errno == EINTR);
                completed = waited < 0 && errno == ECHILD;
                exact = fault.killed && fault.interruptions == parameter && fault.remaining == 0 && fault.wait_calls == parameter + 1;
                if (!completed) { do { waited = waitpid(fault.child, nullptr, 0); } while (waited < 0 && errno == EINTR); if (waited != fault.child) return 2; }
            }
#endif
            const bool no_descriptors = descriptor_count() == before;
            const bool injected = fault.injected && fault.nothrow_seen;
            std::printf("SPAWN_ROLLBACK mode=%u parameter=%u injected=%d error=%d completed_before_fixture_cleanup=%d exact=%d descriptors_restored=%d\n",
                mode, parameter, int(injected), int(error), int(completed), int(exact), int(no_descriptors));
            if (!injected || !error || !completed || !exact || !no_descriptors) passed = false;
        }
    }
    if (!healthy(argv[1]) || descriptor_count() != before) passed = false;
    if (!passed) std::fprintf(stderr,"FAIL: failed spawn must complete owned child rollback before returning\n");
    return passed ? 0 : 1;
}
