#include "protocol.h"
#include "capsid/runtime.h"

#include "win32_compat.h"
#include <signal.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <spawn.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/wait.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern char **environ;

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot open fixture");
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void write_all(int fd, const std::vector<uint8_t> &data) {
    size_t offset = 0;
    while (offset < data.size()) {
#if defined(_WIN32)
        const ssize_t count = capsid::win32::send_fd(
            fd, &data[offset], data.size() - offset, 0);
#elif defined(MSG_NOSIGNAL)
        const ssize_t count =
            send(fd, &data[offset], data.size() - offset, MSG_NOSIGNAL);
#else
        const ssize_t count =
            send(fd, &data[offset], data.size() - offset, 0);
#endif
        if (count <= 0) {
            fail("could not write protocol frame");
        }
        offset += static_cast<size_t>(count);
    }
}

void send_frame(int fd, const capsid::protocol::Frame &frame) {
    std::vector<uint8_t> wire;
    require(capsid::protocol::encode(frame, &wire), "frame encodes");
    write_all(fd, wire);
}

bool read_frame(int fd,
                capsid::protocol::Parser *parser,
                capsid::protocol::Frame *frame,
                int timeout_ms) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const capsid::protocol::ParseResult result = parser->next(frame);
        if (result == capsid::protocol::kParseFrame) {
            return true;
        }
        if (result == capsid::protocol::kParseError) {
            fail("worker response protocol error");
        }
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count());
        if (capsid::win32::capsid_poll(&descriptor, 1, remaining) <= 0) {
            return false;
        }
        uint8_t buffer[4096];
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            return false;
        }
        require(
            parser->append(buffer, static_cast<size_t>(count)),
            "worker response buffered");
    }
}

pid_t spawn_worker(const char *path, int *parent_fd) {
#if defined(_WIN32)
    // Windows manual spawn: socket pair + CreateProcess with the child
    // socket in the explicit inheritable-handle list (the test needs to
    // control the worker fd directly to inject protocol violations).
    if (!capsid::win32::ensure_winsock()) {
        fail("winsock init failed");
    }
    int sockets[2];
    if (!capsid::win32::create_socket_pair(sockets)) {
        fail("socket pair failed");
    }
    HANDLE child_handle = reinterpret_cast<HANDLE>(
        static_cast<intptr_t>(_get_osfhandle(sockets[1])));
    if (!SetHandleInformation(child_handle, HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
        fail("handle inheritance failed");
    }
    const std::string fd_text = std::to_string(
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(child_handle)));
    std::string command_line =
        std::string("\"") + path + "\" --ipc-fd " + fd_text;
    STARTUPINFOEXA startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T attribute_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attribute_size));
    if (startup.lpAttributeList == NULL ||
        !InitializeProcThreadAttributeList(
            startup.lpAttributeList, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(
            startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &child_handle,
            sizeof(child_handle), NULL, NULL)) {
        fail("spawn attribute list failed");
    }
    PROCESS_INFORMATION info;
    std::memset(&info, 0, sizeof(info));
    if (!CreateProcessA(path, &command_line[0], NULL, NULL, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &startup.StartupInfo, &info)) {
        fail("worker spawn failed");
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    CloseHandle(info.hThread);
    close(sockets[1]);
    *parent_fd = sockets[0];
    return static_cast<pid_t>(info.dwProcessId);
#else
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        fail("socketpair failed");
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        fail("spawn actions failed");
    }
    const int child_fd = 3;
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], child_fd);
    if (sockets[1] != child_fd) {
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
    }
    char fd_text[] = "3";
    char *const arguments[] = {
        const_cast<char *>(path),
        const_cast<char *>("--ipc-fd"),
        fd_text,
        NULL,
    };
    pid_t pid = -1;
    const int result =
        posix_spawn(&pid, path, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (result != 0) {
        close(sockets[0]);
        fail("worker spawn failed");
    }
    *parent_fd = sockets[0];
    return pid;
#endif
}

void send_startup(int fd, const std::string &bundle) {
    capsid::protocol::Frame hello;
    hello.type = capsid::protocol::kHello;
    hello.flags = 0;
    hello.request_id = 0;
    capsid::protocol::append_u32(&hello.payload, CAPSID_ABI_VERSION);
    capsid::protocol::append_u64(&hello.payload, 64u * 1024u * 1024u);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, 64);
    capsid::protocol::append_u64(&hello.payload, 5000);
    capsid::protocol::append_u32(&hello.payload, 1024u * 1024u);
    capsid::protocol::append_u32(&hello.payload, 4);
    capsid::protocol::append_u32(&hello.payload, 1024);
    capsid::protocol::append_u32(&hello.payload, 64u * 1024u);
    capsid::protocol::append_u32(&hello.payload, 4u * 1024u * 1024u);
    hello.payload.push_back(0);
    capsid::protocol::append_u32(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, 0);
    capsid::protocol::append_u16(&hello.payload, 0);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u64(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(&hello.payload, 0);
    hello.payload.push_back(0);
    capsid::protocol::append_u32(&hello.payload, 0);
    capsid::protocol::append_u16(&hello.payload, 0);
    capsid::protocol::append_u16(&hello.payload, 0);
    capsid::protocol::append_u16(&hello.payload, 0);
    capsid::protocol::append_u32(&hello.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(&hello.payload, 0);
    send_frame(fd, hello);

    capsid::protocol::Frame load;
    load.type = capsid::protocol::kLoadBundle;
    load.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    load.request_id = 0;
    load.payload.assign(bundle.begin(), bundle.end());
    send_frame(fd, load);
}

void append_string16(std::vector<uint8_t> *output,
                     const std::string &value) {
    capsid::protocol::append_u16(
        output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void append_string32(std::vector<uint8_t> *output,
                     const std::string &value) {
    capsid::protocol::append_u32(
        output, static_cast<uint32_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    int fd = -1;
    const pid_t pid = spawn_worker(argv[1], &fd);
    send_startup(fd, read_file(argv[2]));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    require(read_frame(fd, &parser, &frame, 5000), "READY received");
    require(frame.type == capsid::protocol::kReady, "worker is ready");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = 0;
    head.request_id = 41;
    append_string16(&head.payload, "POST");
    append_string32(&head.payload, "https://example.test/stream");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    require(read_frame(fd, &parser, &frame, 3000), "initial credit received");
    require(
        frame.type == capsid::protocol::kWindowUpdate &&
            frame.request_id == 41,
        "worker advertised request credit");

    capsid::protocol::Frame body;
    body.type = capsid::protocol::kRequestBody;
    body.flags = 0;
    body.request_id = 41;
    body.payload.assign(1025, 0x5a);
    send_frame(fd, body);

    require(read_frame(fd, &parser, &frame, 3000), "violation error received");
    require(
        frame.type == capsid::protocol::kError &&
            frame.request_id == 41,
        "worker rejects request body beyond credit");

    close(fd);
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                 static_cast<DWORD>(pid));
    if (process != NULL) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
#else
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
#endif
    return 0;
}
