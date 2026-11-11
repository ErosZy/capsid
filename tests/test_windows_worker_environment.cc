#include "capsid/runtime.h"
#include "win32_compat.h"
#include "protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Worker {
    capsid_worker *value = nullptr;
    ~Worker() { capsid_worker_destroy(value); }
    capsid_event next() {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) {
            const capsid_result flushed = capsid_worker_flush(value);
            require(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "IPC flush remains available");
            capsid_event event{}; event.struct_size = sizeof(event);
            const capsid_result result = capsid_worker_next_event(value, &event);
            if (result == CAPSID_OK) return event;
            require(result == CAPSID_WOULD_BLOCK, "IPC event read remains available");
            Sleep(1);
        }
        throw std::runtime_error("worker event deadline");
    }
};
struct RestoreEnvironment {
    const wchar_t *name;
    std::vector<wchar_t> previous;
    explicit RestoreEnvironment(const wchar_t *key) : name(key) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size) {
            previous.resize(size);
            require(GetEnvironmentVariableW(name, previous.data(), size) < size, "read original test process environment");
        }
    }
    ~RestoreEnvironment() { SetEnvironmentVariableW(name, previous.empty() ? nullptr : previous.data()); }
};
void actual_worker(const char *path, uint16_t wire) {
    capsid_worker_config config; capsid_worker_config_init(&config);
    config.worker_path = path; config.strict_sandbox = 0;
    capsid_protocol_options options;
    require(capsid_protocol_options_init(&options, sizeof(options)) == CAPSID_OK, "protocol options");
    options.wire_version = wire;
    if (wire == 4) { options.capabilities = 31; options.max_channels = 2; }
    Worker worker;
    require(capsid_worker_spawn_ex(&config, &options, &worker.value) == CAPSID_OK, "actual Worker spawn");
    const char bundle[] = "export default {fetch(){return new Response('ok')}};";
    require(capsid_worker_load_bundle(worker.value, reinterpret_cast<const uint8_t *>(bundle), sizeof(bundle) - 1) == CAPSID_OK, "actual application load");
    require(worker.next().type == CAPSID_EVENT_READY, "actual Worker reaches READY despite poisoned parent SystemRoot");
    require(capsid_worker_begin_bodyless_request(worker.value, 1, "GET", "https://example.test/", nullptr, 0) == CAPSID_OK, "actual request begin");
    require(capsid_worker_grant_response_credit(worker.value, 1, 4096) == CAPSID_OK, "actual response credit");
    std::string body;
    bool ended = false, started = false;
    for (unsigned index = 0; index < 20 && !ended; ++index) {
        const capsid_event event = worker.next();
        require(event.request_id == 1, "actual response request identity");
        if (event.type == CAPSID_EVENT_RESPONSE_HEAD) { require(event.status == 200, "actual response status"); started = true; }
        else if (event.type == CAPSID_EVENT_RESPONSE_BODY) body.append(reinterpret_cast<const char *>(event.payload.data), event.payload.size);
        else if (event.type == CAPSID_EVENT_RESPONSE_END) ended = true;
        else require(event.type == CAPSID_EVENT_REQUEST_CREDIT, "no terminal or unexpected application event");
    }
    require(started && ended && body == "ok", "actual response completes with exact bytes");
    std::printf("actual Worker wire=%u: READY and request passed\n", unsigned(wire));
}

// This self-launched peer inspects only the physical process environment.
// The two real Worker cases above supply the application startup evidence.
int inspect_environment(int fd) {
    if (!capsid::win32::ensure_winsock()) return 2;
    wchar_t expected[MAX_PATH + 1], actual[MAX_PATH + 1];
    const UINT expected_size = GetWindowsDirectoryW(expected, MAX_PATH + 1);
    const DWORD actual_size = GetEnvironmentVariableW(L"SystemRoot", actual, MAX_PATH + 1);
    bool correct = expected_size > 0 && expected_size <= MAX_PATH && actual_size > 0 && actual_size <= MAX_PATH && std::wcscmp(expected, actual) == 0;
    wchar_t *block = GetEnvironmentStringsW();
    unsigned variables = 0;
    if (!block) correct = false;
    else {
        for (const wchar_t *entry = block; *entry; entry += std::wcslen(entry) + 1) {
            ++variables;
            if (_wcsnicmp(entry, L"SystemRoot=", 11) != 0) correct = false;
        }
        FreeEnvironmentStringsW(block);
    }
    correct = correct && variables == 1 && GetEnvironmentVariableW(L"CAPSID_NATIVE_ENV_SENTINEL", nullptr, 0) == 0;
    capsid::protocol::Parser parser;
    capsid::protocol::Frame incoming;
    for (;;) {
        const capsid::protocol::ParseResult result = parser.next(&incoming);
        if (result == capsid::protocol::kParseFrame) break;
        if (result != capsid::protocol::kParseNeedMore) return 2;
        uint8_t bytes[4096]; const ssize_t count = capsid::win32::read_fd(fd, bytes, sizeof(bytes));
        if (count <= 0 || !parser.append(bytes, static_cast<size_t>(count))) return 2;
    }
    if (incoming.type != capsid::protocol::kHello) return 2;
    capsid::protocol::Frame report;
    report.type = capsid::protocol::kError;
    const std::string message = correct ? "platform-environment-ok" : "platform-environment-wrong";
    report.payload.assign(message.begin(), message.end());
    std::vector<uint8_t> bytes;
    if (!capsid::protocol::encode(report, &bytes)) return 2;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = capsid::win32::send_fd(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (count <= 0) return 2;
        offset += static_cast<size_t>(count);
    }
    capsid_pollfd descriptor{}; descriptor.fd = fd; descriptor.events = POLLIN;
    if (capsid::win32::capsid_poll(&descriptor, 1, 5000) <= 0) return 2;
    uint8_t shutdown[128];
    (void)capsid::win32::read_fd(fd, shutdown, sizeof(shutdown));
    return correct ? 0 : 1;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--ipc-fd") == 0) {
        const int fd = _open_osfhandle(static_cast<intptr_t>(std::strtoull(argv[2], nullptr, 10)), _O_RDWR | _O_BINARY);
        return fd < 0 ? 2 : inspect_environment(fd);
    }
    try {
        require(argc == 2, "actual Worker executable argument");
        require(capsid::win32::ensure_winsock(), "parent Winsock initialization");
        const SOCKET warm = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(warm != INVALID_SOCKET, "load the parent socket provider before changing the test process environment");
        closesocket(warm);
        RestoreEnvironment root(L"SystemRoot"), sentinel(L"CAPSID_NATIVE_ENV_SENTINEL");
        require(SetEnvironmentVariableW(L"SystemRoot", L"C:\\capsid-intentionally-missing-windows-directory") != 0, "poison only this test process SystemRoot");
        require(SetEnvironmentVariableW(L"CAPSID_NATIVE_ENV_SENTINEL", L"must-not-inherit") != 0, "set only this test process sentinel");
        actual_worker(argv[1], 3);
        actual_worker(argv[1], 4);
        char self[MAX_PATH + 1];
        const DWORD size = GetModuleFileNameA(nullptr, self, MAX_PATH + 1);
        require(size > 0 && size <= MAX_PATH, "environment probe executable path");
        Worker probe;
        capsid_worker_config config; capsid_worker_config_init(&config);
        config.worker_path = self; config.strict_sandbox = 0;
        require(capsid_worker_spawn(&config, &probe.value) == CAPSID_OK, "physical environment probe spawn");
        const capsid_event event = probe.next();
        require(event.type == CAPSID_EVENT_ERROR && std::string(reinterpret_cast<const char *>(event.payload.data), event.payload.size) == "platform-environment-ok", "only the canonical OS-derived SystemRoot enters the physical environment");
        std::puts("Windows worker environment: two actual application modes and physical environment probe PASS");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
