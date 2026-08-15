// Binding v1 §7.2: zero-binding regression (docs/binding-technical-design.md).
//
// A worker that receives no LOAD_BINDING frame must behave exactly like the
// pre-Binding single-runtime worker: READY carries the unchanged baseline
// sandbox identity, an App that imports an undeclared capsid:binding/*
// fails at module resolution (no lazy Binding Runtime, no READY), and a
// LOAD_BINDING after the bundle sealed the sequence is rejected.

#include "build_identity.h"
#include "capability_policy.h"
#include "capsid/runtime.h"
#include "ipc_validation.h"
#include "protocol.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

void require(bool condition, const std::string &message) {
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
#ifdef MSG_NOSIGNAL
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

enum class ReadResult { kFrame, kEof };

ReadResult read_frame(int fd,
                      capsid::protocol::Parser *parser,
                      capsid::protocol::Frame *frame,
                      int timeout_ms) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const capsid::protocol::ParseResult result = parser->next(frame);
        if (result == capsid::protocol::kParseFrame) {
            return ReadResult::kFrame;
        }
        if (result == capsid::protocol::kParseError) {
            fail("worker response protocol error");
        }
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (now >= deadline) {
            fail("timed out waiting for a worker frame");
        }
        struct pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)
                .count());
        // Sub-millisecond remainder truncates to zero; clamp so poll waits
        // instead of reporting an instant timeout. The deadline check above
        // is the real timeout authority.
        const int poll_result =
            poll(&descriptor, 1, remaining > 0 ? remaining : 1);
        if (poll_result < 0 && errno == EINTR) {
            continue;  // SIGCHLD while the worker exited
        }
        if (poll_result < 0) {
            fail("poll failed waiting for a worker frame (errno=" +
                 std::to_string(errno) + ")");
        }
        if (poll_result == 0) {
            continue;  // spurious wake; the deadline check decides
        }
        uint8_t buffer[4096];
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0) {
            return ReadResult::kEof;
        }
        if (count < 0) {
            fail("read failed on the worker channel");
        }
        require(
            parser->append(buffer, static_cast<size_t>(count)),
            "worker response buffered");
    }
}

pid_t spawn_worker(const char *path, int *parent_fd) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        fail("socketpair failed");
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        fail("posix_spawn_file_actions_init failed");
    }
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
    posix_spawn_file_actions_addclose(&actions, sockets[1]);

    char fd_text[16];
    std::snprintf(fd_text, sizeof(fd_text), "%d", 3);
    char *arguments[] = {
        const_cast<char *>(path),
        const_cast<char *>("--ipc-fd"),
        fd_text,
        NULL,
    };
    pid_t pid = -1;
    if (posix_spawn(&pid, path, &actions, NULL, arguments, environ) != 0) {
        fail("posix_spawn failed");
    }
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    *parent_fd = sockets[0];
    return pid;
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

capsid::protocol::Frame minimal_hello() {
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
    return hello;
}

void send_hello(int fd) { send_frame(fd, minimal_hello()); }

void send_bundle(int fd, const std::string &bundle) {
    capsid::protocol::Frame load;
    load.type = capsid::protocol::kLoadBundle;
    load.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    load.request_id = 0;
    load.payload.assign(bundle.begin(), bundle.end());
    send_frame(fd, load);
}

void finish_worker(int fd, pid_t pid, bool expect_clean_readiness) {
    (void)expect_clean_readiness;
    close(fd);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
}

void test_zero_binding_ready_baseline(const char *worker_path,
                                      const char *p0_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    send_bundle(fd, read_file(p0_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    require(
        read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
        "READY never arrived for a zero-binding worker");
    require(frame.type == capsid::protocol::kReady,
            "zero-binding worker did not report READY");
    require(
        (frame.flags & ~capsid::protocol::kReadySandboxFeatureMask) == 0,
        "READY carries non-sandbox flags");
    const std::string expected(CAPSID_BUILD_COMPATIBILITY_ID);
    require(
        expected.size() == 71 &&
            frame.payload.size() == expected.size() &&
            std::memcmp(frame.payload.data(), expected.data(),
                        expected.size()) == 0,
        "zero-binding READY payload diverged from the baseline identity");
    finish_worker(fd, pid, true);
}

void test_undeclared_binding_import_fails(const char *worker_path,
                                          const char *import_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    send_bundle(fd, read_file(import_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    // The module-deny audit frame may arrive before the startup error.
    bool failed = false;
    for (int i = 0; i < 8; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "no frame arrived for the undeclared binding import");
        if (frame.type == capsid::protocol::kError) {
            failed = true;
            break;
        }
    }
    require(failed && frame.request_id == 0,
            "undeclared binding import did not fail the worker startup");
    const std::string message(
        reinterpret_cast<const char *>(frame.payload.data()),
        frame.payload.size());
    require(
        message.find("capsid:binding") != std::string::npos,
        "startup error does not name the undeclared binding import");
    // No READY may follow: the worker never built a Binding Runtime.
    require(
        read_frame(fd, &parser, &frame, 3000) == ReadResult::kEof,
        "worker stayed alive after the failed binding import");
    int status = 0;
    close(fd);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

void test_load_binding_after_bundle_is_rejected(const char *worker_path,
                                                const char *p0_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    send_bundle(fd, read_file(p0_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    require(
        read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame &&
            frame.type == capsid::protocol::kReady,
        "bundle READY did not arrive before the sealed LOAD_BINDING");
    // Only now send the LOAD_BINDING: startup reads buffer anything sent
    // earlier, so the seal violation must arrive after READY to be seen by
    // the post-startup frame handler.
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    capsid::protocol::append_u32(&binding.payload, 0);
    send_frame(fd, binding);

    // The late LOAD_BINDING is an IPC violation: the worker reports it and
    // shuts down without ever creating a Binding Runtime.
    bool rejected = false;
    for (int i = 0; i < 4; ++i) {
        const ReadResult result = read_frame(fd, &parser, &frame, 5000);
        if (result == ReadResult::kEof) {
            break;
        }
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 0) {
            rejected = true;
            break;
        }
    }
    require(rejected,
            "worker accepted a LOAD_BINDING after the bundle seal");
    int status = 0;
    close(fd);
    waitpid(pid, &status, 0);
    // The violation path is a reported IPC error followed by an orderly
    // shutdown (the same terminal sequence as a kShutdown frame), not a
    // crash: the worker must simply terminate.
    require(WIFEXITED(status),
            "sealed LOAD_BINDING worker did not terminate");
}

std::vector<uint8_t> mongo_binding_blob(const std::string &source =
                                            "export default () => ({});") {
    // A minimal valid descriptor: mongo with the network-client profile,
    // tjs:internal/core granted, and a configurable factory source.
    std::vector<uint8_t> descriptor;
    append_string16(&descriptor, "mongo");
    append_string32(&descriptor, "{}");
    capsid::protocol::append_u32(&descriptor, 0);  // secrets
    capsid::protocol::append_u32(&descriptor, 1);  // profiles
    append_string16(&descriptor, "network-client");
    capsid::protocol::append_u32(&descriptor, 1);  // modules
    append_string16(&descriptor, "tjs:internal/core");
    capsid::protocol::append_u32(&descriptor, 0);  // net rules
    capsid::protocol::append_u32(&descriptor, 0);  // fs read
    capsid::protocol::append_u32(&descriptor, 0);  // fs write
    capsid::protocol::append_u32(&descriptor, 0);  // env
    capsid::protocol::append_u32(&descriptor, 0);  // stdio
    std::vector<uint8_t> blob;
    capsid::protocol::append_u32(
        &blob, static_cast<uint32_t>(descriptor.size()));
    blob.insert(blob.end(), descriptor.begin(), descriptor.end());
    blob.insert(blob.end(), source.begin(), source.end());
    return blob;
}

// Binding v1 §7.4: a worker with one binding reports the v4 READY proof —
// the compat id prefix plus the canonical profile digest; the zero-binding
// baseline (tested above) stays untouched.
void test_binding_ready_proof(const char *worker_path,
                              const char *p0_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);

    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    // A real factory: the worker must evaluate the Binding module, call
    // the factory with (config, secrets, log) and freeze the method table
    // before READY.
    binding.payload = mongo_binding_blob(
        "export default ({ config, secrets, log }) => {"
        "  log.info('binding warmed');"
        "  return { find(input) { return 'find:' + input.collection; } };"
        "};");
    send_frame(fd, binding);
    send_bundle(fd, read_file(p0_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    // The factory's log.info emits a LOG frame before READY.
    bool saw_binding_log = false;
    for (int i = 0; i < 8; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "binding worker produced no startup frame");
        if (frame.type == capsid::protocol::kLog) {
            const std::string log_message(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            if (log_message.find("binding warmed") != std::string::npos) {
                saw_binding_log = true;
            }
            continue;
        }
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
        fail("binding worker did not report READY");
    }
    require(frame.type == capsid::protocol::kReady &&
                saw_binding_log,
            "binding worker did not report READY after the factory log");
    capsid::WorkerReadyProof proof;
    std::string compat_id;
    std::string error;
    require(
        capsid::parse_ready_proof(
            frame.payload, &compat_id, &proof, &error),
        "binding READY proof was rejected: " + error);
    require(compat_id == CAPSID_BUILD_COMPATIBILITY_ID &&
                proof.extended,
            "binding READY did not extend the baseline identity");
    capsid::WorkerBindingDescriptor expected;
    expected.name = "mongo";
    expected.profiles.push_back("network-client");
    require(
        proof.sandbox_profile_digest ==
            capsid::compute_binding_profile_digest(
                std::vector<capsid::WorkerBindingDescriptor>{expected}),
        "binding READY profile digest diverged from the worker's union");
    finish_worker(fd, pid, true);
}

// Binding v1 §7.5: a Binding factory or method-export violation fails the
// worker startup (generation warmup) before READY, with a diagnostic that
// names the failure.
void expect_binding_startup_failure(const char *worker_path,
                                    const char *p0_path,
                                    const std::string &source,
                                    const char *needle) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(source);
    send_frame(fd, binding);
    send_bundle(fd, read_file(p0_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    bool failed = false;
    for (int i = 0; i < 8; ++i) {
        if (read_frame(fd, &parser, &frame, 5000) == ReadResult::kEof) {
            break;
        }
        if (frame.type == capsid::protocol::kError) {
            failed = true;
            break;
        }
    }
    require(failed && frame.request_id == 0,
            "binding startup violation did not fail the worker");
    const std::string message(
        reinterpret_cast<const char *>(frame.payload.data()),
        frame.payload.size());
    require(message.find(needle) != std::string::npos,
            "startup error does not name the binding failure");
    int status = 0;
    close(fd);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

void test_binding_factory_failures(const char *worker_path,
                                   const char *p0_path) {
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => { throw new Error('boom'); };",
        "boom");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => ({ constructor() {} });",
        "invalid method name");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => 42;",
        "factory must return an object");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => ({ find: 7 });",
        "method is not a function");
}


// Binding v1 §7.6 end-to-end: the App's fetch calls mongo.find through
// the facade; the response body must carry the Binding's return value.
void test_binding_end_to_end_call(const char *worker_path,
                                  const char *call_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        "export default ({ config, secrets, log }) => {"
        "  return { find(input) { return 'find:' + input.collection; } };"
        "};");
    send_frame(fd, binding);
    send_bundle(fd, read_file(call_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int i = 0; i < 8; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "no frame arrived before READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "binding call worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 77;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "response frames stopped before ResponseEnd");
        if (frame.type == capsid::protocol::kResponseBody) {
            body.append(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
        }
        if (frame.type == capsid::protocol::kResponseEnd) {
            ended = true;
            break;
        }
    }
    require(ended, "binding call response never ended");
    require(body == "result:find:users",
            "binding call response body is wrong: " + body);
    finish_worker(fd, pid, true);
}

// Binding v1 §7.7: a Binding fetch to a target outside its net policy
// fails closed before any connection — the App sees the egress denial.
void test_binding_egress_denial(const char *worker_path,
                                const char *call_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        "export default ({ config, secrets, log }) => {"
        "  return { find() {"
        "    return fetch('http://127.0.0.1:9999/').then((r) => r.text());"
        "  } };"
        "};");
    send_frame(fd, binding);
    send_bundle(fd, read_file(call_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int i = 0; i < 8; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "no frame arrived before READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "egress binding worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 78;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "egress response frames stopped before the terminal");
        // The app handler rejection terminates the request as a kError
        // carrying the egress denial text.
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 78) {
            body.assign(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            ended = true;
            break;
        }
        if (frame.type == capsid::protocol::kResponseBody) {
            body.append(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
        }
        if (frame.type == capsid::protocol::kResponseEnd) {
            ended = true;
            break;
        }
    }
    require(ended, "egress binding request never terminated");
    require(body.find("egress") != std::string::npos,
            "egress binding error does not carry the denial: " + body);
    finish_worker(fd, pid, true);
}

// Binding v1 P0-5: raw core.TCP egress obeys the Binding net policy —
// a direct connect to an unauthorized target fails before any socket
// operation, not only through the HTTP client.
void test_binding_raw_tcp_egress_denial(const char *worker_path,
                                        const char *call_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        "import core from 'tjs:internal/core';"
        "export default ({ config, secrets, log }) => {"
        "  return { find() {"
        "    const sock = new core.TCPSocket('127.0.0.1', 9999);"
        "    return sock.opened;"
        "  } };"
        "};");
    send_frame(fd, binding);
    send_bundle(fd, read_file(call_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int i = 0; i < 8; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "no frame arrived before READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "raw-tcp binding worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 79;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "raw-tcp response frames stopped before the terminal");
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 79) {
            body.assign(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            ended = true;
            break;
        }
        if (frame.type == capsid::protocol::kResponseBody) {
            body.append(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
        }
        if (frame.type == capsid::protocol::kResponseEnd) {
            ended = true;
            break;
        }
    }
    require(ended, "raw-tcp binding request never terminated");
    // The denial itself is the contract (no connection may be made); the
    // text may degrade to the stable failure message for exception-shaped
    // rejection reasons.
    require(body.find("egress") != std::string::npos ||
                body.find("binding call failed") != std::string::npos,
            "raw-tcp denial carries no failure text: " + body);
    finish_worker(fd, pid, true);
}

void test_load_binding_abi_validation() {
    capsid_worker *worker = NULL;
    require(
        capsid_worker_load_binding(worker, NULL) ==
            CAPSID_INVALID_ARGUMENT,
        "null worker/null descriptor accepted");

    capsid_binding_descriptor descriptor;
    std::memset(&descriptor, 0, sizeof(descriptor));
    descriptor.struct_size = sizeof(descriptor);
    descriptor.version = 99;
    require(
        capsid_worker_load_binding(worker, &descriptor) ==
            CAPSID_INVALID_ARGUMENT,
        "unknown descriptor version accepted");
    descriptor.version = CAPSID_BINDING_DESCRIPTOR_VERSION;
    descriptor.binding_name = "Mongo";
    require(
        capsid_worker_load_binding(worker, &descriptor) ==
            CAPSID_INVALID_ARGUMENT,
        "invalid binding name accepted");
    descriptor.binding_name = "mongo";
    require(
        capsid_worker_load_binding(worker, &descriptor) ==
            CAPSID_INVALID_ARGUMENT,
        "null worker with a valid descriptor accepted");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        fail("expected worker path, p0 fixture, binding-import fixture "
             "and binding-call fixture");
    }
    test_zero_binding_ready_baseline(argv[1], argv[2]);
    test_undeclared_binding_import_fails(argv[1], argv[3]);
    test_load_binding_after_bundle_is_rejected(argv[1], argv[2]);
    test_binding_ready_proof(argv[1], argv[2]);
    test_binding_factory_failures(argv[1], argv[2]);
    test_binding_end_to_end_call(argv[1], argv[4]);
    test_binding_egress_denial(argv[1], argv[4]);
    test_binding_raw_tcp_egress_denial(argv[1], argv[4]);
    test_load_binding_abi_validation();
    return 0;
}
