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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <array>
#include <cctype>
#include <cstdint>
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
        if (frame.type == capsid::protocol::kReady) {
            fail("binding startup violation reached READY");
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

std::vector<uint8_t> binding_blob(
    const std::string &binding_name,
    const std::string &source = "export default () => ({});",
    const std::vector<std::string> &fs_read = {},
    const std::vector<std::string> &profiles = {"network-client"},
    const std::vector<std::string> &modules = {"capsid:internal/core"},
    const std::vector<std::string> &fs_write = {},
    const std::vector<std::string> &stdio = {},
    const std::vector<std::string> &net_rules = {},
    const std::string &config_json = "{}",
    const std::vector<std::pair<std::string, std::string>> &secrets = {}) {
    // A minimal valid descriptor with the selected sandbox profiles,
    // capsid:internal/core granted, and a configurable factory source.
    std::vector<uint8_t> descriptor;
    append_string16(&descriptor, binding_name);
    append_string32(&descriptor, config_json);
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(secrets.size()));
    for (const auto &secret : secrets) {
        append_string16(&descriptor, secret.first);
        capsid::protocol::append_u32(
            &descriptor, static_cast<uint32_t>(secret.second.size()));
        descriptor.insert(descriptor.end(), secret.second.begin(),
                          secret.second.end());
    }
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(profiles.size()));
    for (const std::string &profile : profiles) {
        append_string16(&descriptor, profile);
    }
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(modules.size()));
    for (const std::string &module : modules) {
        append_string16(&descriptor, module);
    }
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(net_rules.size()));
    for (const std::string &rule : net_rules) {
        append_string16(&descriptor, rule);
    }
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(fs_read.size()));
    for (const std::string &path : fs_read) {
        append_string16(&descriptor, path);
    }
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(fs_write.size()));
    for (const std::string &path : fs_write) {
        append_string16(&descriptor, path);
    }
    capsid::protocol::append_u32(&descriptor, 0);  // env
    capsid::protocol::append_u32(
        &descriptor, static_cast<uint32_t>(stdio.size()));
    for (const std::string &stream : stdio) {
        append_string16(&descriptor, stream);
    }
    std::vector<uint8_t> blob;
    capsid::protocol::append_u32(
        &blob, static_cast<uint32_t>(descriptor.size()));
    blob.insert(blob.end(), descriptor.begin(), descriptor.end());
    blob.insert(blob.end(), source.begin(), source.end());
    return blob;
}

std::vector<uint8_t> mongo_binding_blob(
    const std::string &source = "export default () => ({});",
    const std::vector<std::string> &fs_read = {},
    const std::vector<std::string> &profiles = {"network-client"},
    const std::vector<std::string> &modules = {"capsid:internal/core"},
    const std::vector<std::string> &fs_write = {},
    const std::vector<std::string> &stdio = {},
    const std::vector<std::string> &net_rules = {},
    const std::string &config_json = "{}",
    const std::vector<std::pair<std::string, std::string>> &secrets = {}) {
    return binding_blob("mongo", source, fs_read, profiles, modules,
                        fs_write, stdio, net_rules, config_json, secrets);
}

int loopback_tcp_listener(uint16_t *port);
pid_t spawn_websocket_responder(int listener);
void finish_websocket_responder(pid_t child, int listener);

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
        fail("binding worker did not report READY; frame type=" +
             std::to_string(frame.type) + " payload=" +
             std::string(reinterpret_cast<const char *>(frame.payload.data()),
                         frame.payload.size()));
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
                                    const char *needle,
                                    const std::string &config_json = "{}",
                                    const std::vector<std::pair<
                                        std::string, std::string>> &secrets = {}) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        source, {}, {"network-client"}, {"capsid:internal/core"}, {}, {}, {},
        config_json, secrets);
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
            "startup error '" + message + "' does not mention '" +
                needle + "'");
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
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => ({});",
        "between 1 and 128 methods");
    std::string excessive = "export default () => ({";
    for (int index = 0; index < 129; ++index) {
        excessive += "m" + std::to_string(index) + "(){},";
    }
    excessive += "});";
    expect_binding_startup_failure(
        worker_path, p0_path, excessive,
        "between 1 and 128 methods");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => new Proxy({ find() {} }, {"
        " ownKeys() { throw new Error('PROXY_TRAP_RAN'); }"
        "});",
        "Proxy method table");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => { const result = {};"
        " Object.defineProperty(result, 'find', { enumerable: true,"
        "   get() { throw new Error('ACCESSOR_RAN'); } });"
        " return result; };",
        "accessor");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => { const result = { find() {} };"
        " result[Symbol('hidden')] = () => {}; return result; };",
        "symbol");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import core from 'capsid:internal/core';"
        "const socket = new core.TCP();"
        "export default () => ({ find() { return socket; } });",
        "CAPSID_BINDING_OWNER_UNAVAILABLE");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "setTimeout(() => {}, 0);"
        "export default () => ({ find() {} });",
        "CAPSID_BINDING_OWNER_UNAVAILABLE");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import core from 'capsid:internal/core';"
        "export default () => { new core.TCP();"
        " return { find() { return 'unreachable'; } }; };",
        "CAPSID_BINDING_OWNER_UNAVAILABLE");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "export default () => { setTimeout(() => {}, 0);"
        " return { find() {} }; };",
        "CAPSID_BINDING_OWNER_UNAVAILABLE");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import * as utils from 'tjs:utils';"
        "export default () => ({ find() { return utils; } });",
        "module is not authorized for this binding: tjs:utils");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import * as utils from 'capsid:utils';"
        "export default () => ({ find() { return utils; } });",
        "module is not authorized for this binding: capsid:utils");
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

std::string read_response_body(int fd,
                               capsid::protocol::Parser *parser,
                               uint64_t request_id) {
    capsid::protocol::Frame frame;
    std::string body;
    for (int index = 0; index < 128; ++index) {
        require(read_frame(fd, parser, &frame, 5000) ==
                    ReadResult::kFrame,
                "Binding lifecycle response stopped before terminal frame");
        if (frame.request_id != request_id) {
            continue;
        }
        if (frame.type == capsid::protocol::kResponseBody) {
            body.append(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
        }
        if (frame.type == capsid::protocol::kError) {
            return std::string("protocol-error:") +
                   std::string(
                       reinterpret_cast<const char *>(frame.payload.data()),
                       frame.payload.size());
        }
        if (frame.type == capsid::protocol::kResponseEnd) {
            return body;
        }
    }
    fail("Binding lifecycle response never terminated");
    return std::string();
}

void send_bodyless_get(int fd,
                       uint64_t request_id,
                       const std::string &path) {
    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = request_id;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test" + path);
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);
}

// Binding v1 §5.2: metadata is full-width and deadline-based, synchronous
// throws cross with their real message, and calls detached by a completed
// response cannot retain worker/request quota forever.
void test_binding_rpc_lifecycle(const char *worker_path,
                                const char *lifecycle_path) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        "import core from 'capsid:internal/core';"
        "let dispatchCount = 0; let abortStatus = 'not-aborted';"
        "export default () => ({"
        " inspect(_input, call) {"
        "  return [typeof call.requestId, String(call.requestId),"
        "    typeof call.deadline, call.deadline > 4000000000n]"
        "    .join(':');"
        " },"
        " throwSync() { throw new Error('binding-sync-boom'); },"
        " hang(_input, call) { dispatchCount++;"
        "  return new Promise(() => {"
        "   call.signal.addEventListener('abort', () => {"
        "    try { const tcp = new core.TCP(); tcp.close();"
        "      abortStatus = 'owner-ok'; }"
        "    catch (error) { abortStatus = error.message; }"
        "   });"
        "  });"
        " },"
        " dispatchCount() { return dispatchCount; },"
        " abortStatus() { return abortStatus; },"
        " ping() { return 'pong'; }"
        "});");
    send_frame(fd, binding);
    send_bundle(fd, read_file(lifecycle_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int index = 0; index < 8; ++index) {
        require(read_frame(fd, &parser, &frame, 5000) ==
                    ReadResult::kFrame,
                "no frame arrived before lifecycle READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "Binding lifecycle worker did not report READY");

    send_bodyless_get(fd, UINT64_MAX, "/inspect");
    require(read_response_body(fd, &parser, UINT64_MAX) ==
                "result:bigint:18446744073709551615:bigint:true",
            "Binding call metadata lost width or absolute deadline");

    send_bodyless_get(fd, 1, "/throw");
    require(read_response_body(fd, &parser, 1) ==
                "error:Error: binding-sync-boom",
            "synchronous Binding throw lost its message");

    // 17 * 64 exceeds the worker-wide quota if response-terminal calls are
    // not synchronously reclaimed.  Every ignored promise has a rejection
    // handler, so the test observes lifecycle accounting, not unhandled jobs.
    for (uint64_t request_id = 2; request_id < 19; ++request_id) {
        send_bodyless_get(fd, request_id, "/detach");
        require(read_response_body(fd, &parser, request_id) == "detached",
                "detached-call request failed");
    }
    send_bodyless_get(fd, 19, "/probe");
    require(read_response_body(fd, &parser, 19) == "probe:pong",
            "terminal Binding calls retained worker quota");

    // Drive 16 full per-request batches through the dispatched state. A
    // never-settling Binding Promise must still be aborted and reclaimed on
    // CANCEL; otherwise the 1024-entry worker quota blocks a later count call.
    for (uint64_t batch = 0; batch < 16; ++batch) {
        const uint64_t hanging_id = 100 + batch * 2;
        const uint64_t count_id = hanging_id + 1;
        send_bodyless_get(fd, hanging_id, "/await-many");
        send_bodyless_get(fd, count_id, "/dispatch-count");
        require(read_response_body(fd, &parser, count_id) ==
                    "count:" + std::to_string((batch + 1) * 64),
                "dispatched Binding calls retained worker quota");
        capsid::protocol::Frame cancel;
        cancel.type = capsid::protocol::kCancel;
        cancel.flags = 0;
        cancel.request_id = hanging_id;
        send_frame(fd, cancel);
    }
    send_bodyless_get(fd, 200, "/abort-status");
    require(read_response_body(fd, &parser, 200) == "abort:owner-ok",
            "Binding abort listener lost its immutable owner identity");
    finish_worker(fd, pid, true);
}

// Binding v1 §5.3: a synchronous CPU loop inside a Binding method is
// interruptible. The Binding Runtime handler observes the call deadline,
// QuickJS aborts JS_Call, the call settles as an error, and the worker
// enters poison/EXIT instead of hanging forever.
void test_binding_sync_loop_interrupts(const char *worker_path,
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
        "export default () => ({"
        "  spin() { while (true) {} }"
        "});",
        {}, {}, {"capsid:utils"}, {}, {}, {}, "{}", {});
    send_frame(fd, binding);
    send_bundle(fd, read_file(call_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int index = 0; index < 8; ++index) {
        require(read_frame(fd, &parser, &frame, 5000) ==
                    ReadResult::kFrame,
                "no frame arrived before interrupt-test READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "interrupt-test Binding worker did not report READY");

    send_bodyless_get(fd, 1, "/");
    bool saw_error = false;
    bool saw_exit = false;
    for (int index = 0; index < 64; ++index) {
        const ReadResult read = read_frame(fd, &parser, &frame, 5000);
        if (read == ReadResult::kEof) {
            saw_exit = true;
            break;
        }
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 1) {
            saw_error = true;
        }
        if (frame.type == capsid::protocol::kExit) {
            saw_exit = true;
            break;
        }
    }
    require(saw_error || saw_exit,
            "Binding CPU loop was neither interrupted nor worker-exited");
    finish_worker(fd, pid, saw_error);
}

std::string invoke_binding_method(
    const char *worker_path,
    const char *call_path,
    const std::string &source,
    const std::vector<std::string> &fs_read,
    const std::vector<std::string> &profiles,
    const std::vector<std::string> &modules,
    uint64_t request_id,
    const std::vector<std::string> &fs_write = {},
    const std::vector<std::string> &stdio = {},
    const std::vector<std::string> &net_rules = {},
    const std::string &config_json = "{}",
    const std::vector<std::pair<std::string, std::string>> &secrets = {},
    std::vector<capsid::protocol::Frame> *observed_logs = nullptr) {
    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);
    capsid::protocol::Frame binding;
    binding.type = capsid::protocol::kLoadBinding;
    binding.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    binding.request_id = 0;
    binding.payload = mongo_binding_blob(
        source, fs_read, profiles, modules, fs_write, stdio, net_rules,
        config_json, secrets);
    send_frame(fd, binding);
    send_bundle(fd, read_file(call_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int i = 0; i < 8; ++i) {
        require(read_frame(fd, &parser, &frame, 5000) ==
                    ReadResult::kFrame,
                "no frame arrived before Binding READY");
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
        if (frame.type == capsid::protocol::kLog && observed_logs != nullptr) {
            observed_logs->push_back(frame);
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "Binding invocation worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = request_id;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        const ReadResult read = read_frame(fd, &parser, &frame, 5000);
        if (read != ReadResult::kFrame) {
            int status = 0;
            waitpid(pid, &status, 0);
            fail("Binding invocation worker exited before terminal frame "
                 "for request " + std::to_string(request_id) +
                 " with status " + std::to_string(status));
        }
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == request_id) {
            body.assign(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            ended = true;
            break;
        }
        if (frame.type == capsid::protocol::kLog) {
            if (observed_logs != nullptr) {
                observed_logs->push_back(frame);
            }
            continue;
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
    require(ended, "Binding invocation never terminated");
    finish_worker(fd, pid, true);
    return body;
}

void test_binding_init_object_contract(const char *worker_path,
                                       const char *call_path) {
    const std::string source =
        "export default init => {"
        " const { config, secrets, log } = init;"
        " const plain = value => Object.getPrototypeOf(value) === null &&"
        "   Object.isFrozen(value);"
        " if (!plain(init) || !plain(config) || !plain(config.nested) ||"
        "     !Object.isFrozen(config.items) || !plain(config.items[0]) ||"
        "     !plain(secrets) || !Object.isFrozen(log))"
        "   throw new Error('INIT_OBJECT_CONTRACT');"
        " return { find() { return config.nested.value + ':' +"
        "   config.items[0].name + ':' + secrets.password; } };"
        "};";
    const std::string body = invoke_binding_method(
        worker_path, call_path, source, {}, {"network-client"},
        {"capsid:internal/core"}, 106, {}, {}, {},
        R"json({"items":[{"name":"item"}],"nested":{"value":"ok"}})json",
        {{"password", "secret-value"}});
    require(body == "result:ok:item:secret-value",
            "Binding init object contract is wrong: " + body);
}

void test_binding_method_table_is_frozen(const char *worker_path,
                                         const char *call_path) {
    const std::string body = invoke_binding_method(
        worker_path, call_path,
        "export default () => {"
        " const table = { find() { return 'original'; } };"
        " Promise.resolve().then(() => {"
        "   try { table.find = () => 'mutated'; } catch (_) {}"
        " });"
        " return table;"
        "};",
        {}, {"network-client"}, {"capsid:internal/core"}, 107);
    require(body == "result:original",
            "Binding method table was mutable after discovery: " + body);
}

void test_binding_log_envelope_and_redaction(const char *worker_path,
                                             const char *call_path) {
    std::vector<capsid::protocol::Frame> logs;
    const std::string body = invoke_binding_method(
        worker_path, call_path,
        "export default ({ secrets, log }) => ({ find() {"
        " let rejected = false;"
        " try { log.info('invalid', []); }"
        " catch (error) { rejected = error instanceof TypeError; }"
        " if (!rejected) throw new Error('LOG_FIELDS_NOT_REJECTED');"
        " log.warn('password=' + secrets.password,"
        "   { password: secrets.password, safe: 'visible' });"
        " return 'logged';"
        "} });",
        {}, {"network-client"}, {"capsid:internal/core"}, 108,
        {}, {}, {}, "{}", {{"password", "SECRET_CANARY_108"}}, &logs);
    require(body == "result:logged", "Binding log call failed: " + body);
    require(logs.size() == 1, "Binding log call did not emit exactly one log");
    const capsid::protocol::Frame &log = logs[0];
    const std::string payload(
        reinterpret_cast<const char *>(log.payload.data()),
        log.payload.size());
    require(log.flags == capsid::protocol::kFlagBindingLog &&
                log.request_id == 108 &&
                payload.find("mongo") != std::string::npos &&
                payload.find("warn") != std::string::npos &&
                payload.find("visible") != std::string::npos &&
                payload.find("[REDACTED]") != std::string::npos &&
                payload.find("SECRET_CANARY_108") == std::string::npos,
            "Binding log envelope lacks metadata or leaked a secret");
}

// Binding v1 §3.1/§5.1/§7.3: every Binding package runs in its own
// QuickJS runtime/context. A value stashed on one Binding's globalThis must
// stay invisible to another equally-permitted Binding — including a native
// handle, the factory `log` object and a mutated builtin module. The owner
// Binding must keep its own global and module-cache state untouched and be
// able to reuse and close its resources afterwards.
void test_binding_isolation_globals_modules_and_handles(
    const char *worker_path,
    const char *owner_path) {
    char file_path[] = "/tmp/capsid-binding-owner-XXXXXX";
    const int fixture = mkstemp(file_path);
    require(fixture >= 0, "could not create Binding owner fixture");
    close(fixture);

    uint16_t websocket_port = 0;
    const int websocket_listener =
        loopback_tcp_listener(&websocket_port);
    const pid_t websocket_server =
        spawn_websocket_responder(websocket_listener);
    const std::string websocket_target =
        "127.0.0.1:" + std::to_string(websocket_port);

    int fd = -1;
    const pid_t pid = spawn_worker(worker_path, &fd);
    send_hello(fd);

    capsid::protocol::Frame mongo;
    mongo.type = capsid::protocol::kLoadBinding;
    mongo.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    mongo.request_id = 0;
    mongo.payload = binding_blob(
        "mongo",
        std::string("import core from 'capsid:internal/core';") +
            "import { Database } from 'capsid:sqlite';" +
            "import getopts from 'capsid:getopts';" +
            "const wasmBytes = new Uint8Array([" +
            "0,97,115,109,1,0,0,0,1,4,1,96,0,0," +
            "3,2,1,0,7,8,1,4,110,111,111,112,0,0,10,4,1,2,0,11]);" +
            "let owned; export default ({ log }) => ({" +
            " async store() {" +
            "  const file = await core.fs.open('" + file_path + "','r');" +
            "  const tcp = new core.TCP();" +
            "  const udp = new core.UDP();" +
            "  const tls = new core.TLSTcp({isServer:false,verifyPeer:false});" +
            "  const database = new Database(':memory:');" +
            "  database.exec('CREATE TABLE owned(value INTEGER)');" +
            "  const statement = database.prepare('SELECT 1 AS value');" +
            "  const module = new WebAssembly.Module(wasmBytes);" +
            "  const instance = new WebAssembly.Instance(module);" +
            "  const http = new core.HttpClient(); http.timeout = 17;" +
            "  const ws = new core.WebSocket('ws://" + websocket_target +
            "/', null, [], []);" +
            "  const watcher = core.watch('" + file_path + "', () => {});" +
            "  const timer = setTimeout(() => {}, 60000);" +
            "  owned = {file,tcp,udp,tls,database,statement,module,instance," +
            "           http,ws,watcher,timer};" +
            "  getopts.__capsidModuleMarker = 'mongo-module';" +
            "  globalThis.__capsidOwned = owned;" +
            "  globalThis.__capsidLog = log;" +
            "  globalThis.__capsidProbe = 'mongo-global';" +
            "  return 'stored:' + globalThis.__capsidProbe + ':' +" +
            "    getopts.__capsidModuleMarker;" +
            " }," +
            " async cleanup() {" +
            "  const probe = globalThis.__capsidProbe;" +
            "  const moduleMarker = getopts.__capsidModuleMarker;" +
            "  await owned.file.close();" +
            "  owned.tcp.close(); owned.udp.close(); owned.tls.close();" +
            "  owned.statement.all(); owned.statement.finalize();" +
            "  owned.database.exec('SELECT 1'); owned.database.close();" +
            "  WebAssembly.Module.exports(owned.module);" +
            "  void owned.instance.exports; void owned.http.timeout;" +
            "  owned.ws.close(1000, '');" +
            "  void owned.watcher.path; owned.watcher.close();" +
            "  clearTimeout(owned.timer);" +
            "  delete globalThis.__capsidOwned;" +
            "  delete globalThis.__capsidLog;" +
            "  delete globalThis.__capsidProbe;" +
            "  delete getopts.__capsidModuleMarker;" +
            "  return 'closed:' + probe + ':' + moduleMarker;" +
            " }" +
            "});",
        {file_path},
        {"filesystem-read", "filesystem-watch", "network-client", "sqlite"},
        {"capsid:internal/core", "capsid:sqlite", "capsid:getopts"},
        {}, {}, {websocket_target});
    send_frame(fd, mongo);

    capsid::protocol::Frame redis;
    redis.type = capsid::protocol::kLoadBinding;
    redis.flags =
        capsid::protocol::kFlagStart | capsid::protocol::kFlagEnd;
    redis.request_id = 0;
    redis.payload = binding_blob(
        "redis",
        "import getopts from 'capsid:getopts';"
        "export default () => ({ async use() {"
        " const crossOwned = globalThis.__capsidOwned;"
        " const crossProbe = globalThis.__capsidProbe;"
        " const crossLog = globalThis.__capsidLog;"
        " const crossModuleMarker = getopts.__capsidModuleMarker;"
        " let logResult;"
        " try { crossLog.info('cross-binding probe');"
        "   logResult = 'CALLED'; }"
        " catch (error) { logResult = error instanceof TypeError"
        "   ? 'type-error' : 'wrong-error:' + String(error); }"
        " globalThis.__capsidProbe = 'redis-global';"
        " getopts.__capsidModuleMarker = 'redis-module';"
        " return ['isolated',"
        "   String(crossOwned === undefined),"
        "   String(crossProbe === undefined),"
        "   String(crossLog === undefined),"
        "   String(crossModuleMarker === undefined),"
        "   logResult"
        " ].join(':');"
        "} });",
        {file_path},
        {"filesystem-read", "filesystem-watch", "network-client", "sqlite"},
        {"capsid:internal/core", "capsid:sqlite", "capsid:getopts"},
        {}, {}, {websocket_target});
    send_frame(fd, redis);
    send_bundle(fd, read_file(owner_path));

    capsid::protocol::Parser parser;
    capsid::protocol::Frame frame;
    for (int i = 0; i < 12; ++i) {
        if (read_frame(fd, &parser, &frame, 5000) == ReadResult::kEof) {
            int worker_status = 0;
            waitpid(pid, &worker_status, 0);
            fail("owner-test worker exited before READY with status " +
                 std::to_string(worker_status));
        }
        if (frame.type == capsid::protocol::kError) {
            const std::string message(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            fail("owner-test worker startup failed: " + message);
        }
        if (frame.type == capsid::protocol::kReady) {
            break;
        }
    }
    require(frame.type == capsid::protocol::kReady,
            "owner-test worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 99;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(read_frame(fd, &parser, &frame, 5000) ==
                    ReadResult::kFrame,
                "owner-test response stopped before terminal frame");
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == head.request_id) {
            const std::string message(
                reinterpret_cast<const char *>(frame.payload.data()),
                frame.payload.size());
            fail("owner-test Binding call failed: " + message);
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
    unlink(file_path);
    require(ended, "owner-test response never ended");
    require(
        body ==
            "result:stored:mongo-global:mongo-module:"
            "isolated:true:true:true:true:type-error:"
            "closed:mongo-global:mongo-module",
        "a Binding value crossed the per-Binding isolation boundary: " +
            body);
    finish_worker(fd, pid, true);
    finish_websocket_responder(websocket_server, websocket_listener);
}

// Binding v1 §3.3: granting the controlled core must not expose a server,
// fd-adoption, local pipe, or listener surface. This is a JavaScript reachability
// check and therefore holds even when seccomp is disabled (for example Darwin).
void test_binding_core_is_client_only(const char *worker_path,
                                      const char *call_path) {
    const std::string body = invoke_binding_method(
        worker_path,
        call_path,
        "import core from 'capsid:internal/core';"
        "const has = (value, name) => !!value &&"
        "  typeof value[name] === 'function';"
        "export default () => ({ find() {"
        "  let tcp, tls, udp;"
        "  try { tcp = new core.TCP(); }"
        "  catch (error) { return 'tcp-constructor:' + error.message; }"
        "  try { tls = new core.TLSTcp({isServer:false,verifyPeer:false}); }"
        "  catch (error) { return 'tls-constructor:' + error.message; }"
        "  try { udp = new core.UDP(); }"
        "  catch (error) { return 'udp-constructor:' + error.message; }"
        "  return ["
        "    has(tcp, 'bind'), has(tcp, 'listen'),"
        "    has(tcp, 'fileno'), !!core.Pipe, !!core.TTY,"
        "    has(tls, 'bind'), has(tls, 'listen'),"
        "    has(udp, 'bind'), has(udp, 'fileno'),"
        "    !!globalThis.tjs, !!core.fs.File,"
        "    !!core.fs.fileno, !!core.stdoutPrint,"
        "    !!core.stderrPrint, !!core.guessHandle,"
        "    !!core.STDIN_FILENO, !!core.STDOUT_FILENO,"
        "    !!core.STDERR_FILENO,"
        "    typeof (function () {}).bind !== 'function'"
        "  ].map(Number).join('');"
        "} });",
        {},
        {"network-client"},
        {"capsid:internal/core"},
        83);
    require(body == "result:0000000000000000000",
            "Binding core exposes a server/fd surface: " + body);
}

// capsid:readline is grantable only as a pure transformer over caller-provided
// Web Streams. It must not gain ambient stdin/stdout through a hidden tjs
// global, while a memory-backed line read remains usable.
void test_binding_readline_has_no_ambient_stdio(const char *worker_path,
                                                const char *call_path) {
    const std::string body = invoke_binding_method(
        worker_path,
        call_path,
        "import { createInterface, isColorSupported } from 'capsid:readline';"
        "export default () => ({ async find() {"
        "  if (globalThis.tjs !== undefined || isColorSupported())"
        "    return 'ambient-stdio';"
        "  const bytes = new TextEncoder().encode('hello\\n');"
        "  const input = new ReadableStream({ start(c) {"
        "    c.enqueue(bytes); c.close();"
        "  } });"
        "  let written = '';"
        "  const output = new WritableStream({ write(chunk) {"
        "    written += new TextDecoder().decode(chunk);"
        "  } });"
        "  const lines = createInterface({ input, output, prompt: '',"
        "    terminal: false });"
        "  const line = await lines.readline();"
        "  lines.close();"
        "  return line + ':' + written;"
        "} });",
        {},
        {"network-client"},
        {"capsid:internal/core", "capsid:readline"},
        91);
    require(body == "result:hello:",
            "readline acquired ambient stdio or failed pure streams: " +
                body);
}

int loopback_tcp_listener(uint16_t *port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "could not create TCP listener");
    int reuse = 1;
    require(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse)) == 0,
            "could not configure TCP listener");
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(fd, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) == 0,
            "could not bind TCP listener");
    require(listen(fd, 4) == 0, "could not listen on TCP socket");
    socklen_t size = sizeof(address);
    require(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) ==
                0,
            "could not inspect TCP listener");
    *port = ntohs(address.sin_port);
    return fd;
}

pid_t spawn_http_responder(int listener, const std::string &response) {
    const pid_t child = fork();
    require(child >= 0, "could not fork HTTP responder");
    if (child == 0) {
        alarm(15);
        const int client = accept(listener, NULL, NULL);
        if (client < 0) {
            _exit(2);
        }
        char request[4096];
        (void)read(client, request, sizeof(request));
        size_t offset = 0;
        while (offset < response.size()) {
            const ssize_t written = write(
                client, response.data() + offset,
                response.size() - offset);
            if (written <= 0) {
                close(client);
                _exit(3);
            }
            offset += static_cast<size_t>(written);
        }
        close(client);
        close(listener);
        _exit(0);
    }
    return child;
}

void finish_http_responder(pid_t child,
                           int listener,
                           bool expect_request) {
    close(listener);
    if (!expect_request) {
        kill(child, SIGKILL);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (expect_request) {
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "HTTP responder did not serve its expected request");
    }
}

uint32_t rotate_left(uint32_t value, unsigned count) {
    return (value << count) | (value >> (32u - count));
}

std::array<uint8_t, 20> sha1(const std::string &input) {
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8u;
    data.push_back(0x80);
    while (data.size() % 64u != 56u) {
        data.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<uint8_t>(bit_length >> shift));
    }

    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xefcdab89u;
    uint32_t h2 = 0x98badcfeu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xc3d2e1f0u;
    for (size_t offset = 0; offset < data.size(); offset += 64u) {
        uint32_t words[80] = {};
        for (size_t i = 0; i < 16; ++i) {
            const size_t at = offset + i * 4u;
            words[i] = (static_cast<uint32_t>(data[at]) << 24u) |
                       (static_cast<uint32_t>(data[at + 1]) << 16u) |
                       (static_cast<uint32_t>(data[at + 2]) << 8u) |
                       static_cast<uint32_t>(data[at + 3]);
        }
        for (size_t i = 16; i < 80; ++i) {
            words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^
                                       words[i - 14] ^ words[i - 16],
                                   1);
        }
        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (size_t i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcu;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6u;
            }
            const uint32_t next = rotate_left(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = next;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest = {};
    const uint32_t hashes[] = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(hashes[i] >> 24u);
        digest[i * 4 + 1] = static_cast<uint8_t>(hashes[i] >> 16u);
        digest[i * 4 + 2] = static_cast<uint8_t>(hashes[i] >> 8u);
        digest[i * 4 + 3] = static_cast<uint8_t>(hashes[i]);
    }
    return digest;
}

std::string base64(const std::array<uint8_t, 20> &data) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(28);
    for (size_t i = 0; i < data.size(); i += 3) {
        const size_t remaining = data.size() - i;
        const uint32_t value =
            (static_cast<uint32_t>(data[i]) << 16u) |
            (remaining > 1 ? static_cast<uint32_t>(data[i + 1]) << 8u : 0) |
            (remaining > 2 ? static_cast<uint32_t>(data[i + 2]) : 0);
        output.push_back(alphabet[(value >> 18u) & 63u]);
        output.push_back(alphabet[(value >> 12u) & 63u]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6u) & 63u] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 63u] : '=');
    }
    return output;
}

bool ascii_prefix_equal(const std::string &line, const char *prefix) {
    const size_t prefix_size = std::strlen(prefix);
    if (line.size() < prefix_size) {
        return false;
    }
    for (size_t i = 0; i < prefix_size; ++i) {
        const unsigned char left = static_cast<unsigned char>(line[i]);
        const unsigned char right = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

pid_t spawn_websocket_responder(int listener) {
    const pid_t child = fork();
    require(child >= 0, "could not fork WebSocket responder");
    if (child == 0) {
        alarm(15);
        const int client = accept(listener, NULL, NULL);
        if (client < 0) {
            _exit(2);
        }
        std::string request;
        char chunk[1024];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() <= 16u * 1024u) {
            const ssize_t count = read(client, chunk, sizeof(chunk));
            if (count <= 0) {
                close(client);
                _exit(3);
            }
            request.append(chunk, static_cast<size_t>(count));
        }
        const char key_header[] = "sec-websocket-key:";
        std::string key;
        size_t line_start = 0;
        while (line_start < request.size()) {
            const size_t line_end = request.find("\r\n", line_start);
            if (line_end == std::string::npos) {
                break;
            }
            const std::string line =
                request.substr(line_start, line_end - line_start);
            if (ascii_prefix_equal(line, key_header)) {
                size_t value_start = std::strlen(key_header);
                while (value_start < line.size() &&
                       std::isspace(static_cast<unsigned char>(line[value_start]))) {
                    ++value_start;
                }
                key = line.substr(value_start);
                break;
            }
            line_start = line_end + 2;
        }
        if (key.empty()) {
            close(client);
            _exit(4);
        }
        const std::string accept = base64(sha1(
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        size_t sent = 0;
        while (sent < response.size()) {
            const ssize_t count =
                write(client, response.data() + sent, response.size() - sent);
            if (count <= 0) {
                close(client);
                _exit(5);
            }
            sent += static_cast<size_t>(count);
        }
        // The client closes from its onopen callback. Waiting for one frame
        // keeps the handshake alive long enough to prove that callback ran.
        (void)read(client, chunk, sizeof(chunk));
        close(client);
        close(listener);
        _exit(0);
    }
    return child;
}

void finish_websocket_responder(pid_t child, int listener) {
    close(listener);
    int status = 0;
    waitpid(child, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "WebSocket responder did not complete a valid handshake");
}

int loopback_udp_receiver(uint16_t *port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    require(fd >= 0, "could not create UDP receiver");
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(fd, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) == 0,
            "could not bind UDP receiver");
    socklen_t size = sizeof(address);
    require(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) ==
                0,
            "could not inspect UDP receiver");
    *port = ntohs(address.sin_port);
    return fd;
}

void test_binding_open_file_has_no_descriptor_surface(
    const char *worker_path,
    const char *call_path) {
    char file_path[] = "/tmp/capsid-binding-file-handle-XXXXXX";
    const int file = mkstemp(file_path);
    require(file >= 0, "could not create file-handle fixture");
    close(file);
    const std::string body = invoke_binding_method(
        worker_path,
        call_path,
        std::string("import core from 'capsid:internal/core';") +
            "export default () => ({ async find() {" +
            "  const file = await core.fs.open('" + file_path + "','r');" +
            "  const result = typeof file.fileno;" +
            "  await file.close(); return 'fileno:' + result;" +
            "} });",
        {file_path},
        {"filesystem-read"},
        {"capsid:internal/core"},
        94);
    unlink(file_path);
    require(body == "result:fileno:undefined",
            "an opened File leaked its host descriptor: " + body);
}

void test_binding_dns_egress_gate(const char *worker_path,
                                  const char *call_path) {
    const std::string source =
        "import core from 'capsid:internal/core';"
        "export default () => ({ async find() {"
        "  try {"
        "    const result = await core.getaddrinfo('localhost',{family:0});"
        "    return 'dns-allowed:' + (result.length > 0);"
        "  } catch (error) { return 'dns-denied:' + error.message; }"
        "} });";
    const std::string denied = invoke_binding_method(
        worker_path,
        call_path,
        source,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        95);
    require(denied.find(
                "result:dns-denied:CAPSID_DNS_EGRESS_DENIED:") == 0,
            "DNS resolution bypassed the Binding net policy: " + denied);

    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        source,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        96,
        {},
        {},
        {"localhost:443"});
    require(allowed == "result:dns-allowed:true",
            "DNS rejected a host covered by a Binding net grant: " +
                allowed);
}

std::string wasi_fd_close_binding_source(const std::string &options,
                                         bool run_start = true) {
    // The module imports fd_close, exports one page of memory (required by
    // WAMR WASI), and exports _start. Its preopen is guest fd 3.
    return
        "import WASI from 'capsid:wasi';"
        "const bytes = new Uint8Array(["
        "0,97,115,109,1,0,0,0,"
        "1,9,2,96,1,127,1,127,96,0,0,"
        "2,35,1,22,119,97,115,105,95,115,110,97,112,115,104,111,116,95,112,114,101,118,105,101,119,49,"
        "8,102,100,95,99,108,111,115,101,0,0,"
        "3,2,1,1,5,3,1,0,1,"
        "7,19,2,6,95,115,116,97,114,116,0,1,"
        "6,109,101,109,111,114,121,2,0,"
        "10,9,1,7,0,65,3,16,0,26,11]);"
        "export default () => ({ find() {"
        "  try {"
        "    const wasi = new WASI(" + options + ");"
        "    const module = new WebAssembly.Module(bytes);"
        "    const instance = new WebAssembly.Instance("
        "      module, wasi.getImportObject());" +
        (run_start ? "wasi.start(instance);" : "") +
        "    return 'wasi-allowed';"
        "  } catch (error) {"
        "    return 'wasi-denied:' + error.message;"
        "  }"
        "} });";
}

void test_binding_wasi_preopen_is_policy_gated(const char *worker_path,
                                               const char *call_path) {
    const std::string body = invoke_binding_method(
        worker_path,
        call_path,
        wasi_fd_close_binding_source(
            "{version:'wasi_snapshot_preview1',"
            "preopens:{'/guest':'/tmp'}}"),
        {},
        {"wasi"},
        {"capsid:internal/core", "capsid:wasi"},
        84);
    require(body.find(
                "result:wasi-denied:CAPSID_WASI_FS_DENIED:") == 0,
            "WASI opened an undeclared host path: " + body);
}

void test_binding_wasi_allowed_preopen_runs(const char *worker_path,
                                            const char *call_path) {
    const std::string body = invoke_binding_method(
        worker_path,
        call_path,
        wasi_fd_close_binding_source(
            "{version:'wasi_snapshot_preview1',"
            "preopens:{'/guest':'/tmp'}}"),
        {},
        {"filesystem-write", "wasi"},
        {"capsid:internal/core", "capsid:wasi"},
        85,
        {"/tmp"});
    require(body == "result:wasi-allowed",
            "WASI rejected a declared writable preopen: " + body);
}

void test_binding_wasi_stdio_is_policy_gated(const char *worker_path,
                                             const char *call_path) {
    const std::string denied = invoke_binding_method(
        worker_path,
        call_path,
        wasi_fd_close_binding_source(
            "{version:'wasi_snapshot_preview1',stdin:0}", false),
        {},
        {"wasi"},
        {"capsid:internal/core", "capsid:wasi"},
        86);
    require(denied.find(
                "result:wasi-denied:CAPSID_WASI_STDIO_DENIED:") == 0,
            "WASI attached undeclared stdin: " + denied);

    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        wasi_fd_close_binding_source(
            "{version:'wasi_snapshot_preview1',stdout:1}", false),
        {},
        {"wasi"},
        {"capsid:internal/core", "capsid:wasi"},
        87,
        {},
        {"stdout"});
    require(allowed == "result:wasi-allowed",
            "WASI rejected declared stdout: " + allowed);
}

void test_binding_sqlite_paths_and_extensions_are_gated(
    const char *worker_path,
    const char *call_path) {
    const std::string memory = invoke_binding_method(
        worker_path,
        call_path,
        "import { Database } from 'capsid:sqlite';"
        "export default () => ({ find() {"
        "  try {"
        "    const db = new Database(':memory:');"
        "    db.exec('CREATE TABLE ok(value INTEGER)');"
        "    try { db.loadExtension('/tmp/capsid-no-extension'); }"
        "    catch (error) { db.close(); return 'extension:' + error.message; }"
        "    db.close(); return 'extension-loaded';"
        "  } catch (error) { return 'open:' + error.message; }"
        "} });",
        {},
        {"sqlite"},
        {"capsid:internal/core", "capsid:sqlite"},
        88);
    require(memory.find(
                "result:extension:CAPSID_SQLITE_EXTENSION_DENIED") == 0,
            "SQLite memory/extension boundary is wrong: " + memory);

    const std::string denied = invoke_binding_method(
        worker_path,
        call_path,
        "import { Database } from 'capsid:sqlite';"
        "export default () => ({ find() {"
        "  try { new Database('/tmp/capsid-sqlite-denied.db').close();"
        "    return 'opened'; }"
        "  catch (error) { return 'denied:' + error.message; }"
        "} });",
        {},
        {"filesystem-write", "sqlite"},
        {"capsid:internal/core", "capsid:sqlite"},
        89);
    require(denied.find("result:denied:fs denied") == 0,
            "SQLite opened an undeclared database path: " + denied);

    char path[] = "/tmp/capsid-binding-sqlite-XXXXXX";
    const int temporary_fd = mkstemp(path);
    require(temporary_fd >= 0, "could not reserve SQLite test path");
    close(temporary_fd);
    unlink(path);
    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        std::string("import { Database } from 'capsid:sqlite';") +
            "export default () => ({ find() {" +
            "  const db = new Database('" + path + "');" +
            "  db.exec('CREATE TABLE ok(value INTEGER)');" +
            "  db.close(); return 'sqlite-allowed';" +
            "} });",
        {},
        {"filesystem-write", "sqlite"},
        {"capsid:internal/core", "capsid:sqlite"},
        90,
        {path});
    unlink(path);
    unlink((std::string(path) + "-journal").c_str());
    unlink((std::string(path) + "-wal").c_str());
    unlink((std::string(path) + "-shm").c_str());
    require(allowed == "result:sqlite-allowed",
            "SQLite rejected a declared database path: " + allowed);
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

// Binding v1 §7.7: exercise the actual raw core.TCP constructor and connect
// entry point. A deny must be the native policy error (not an unrelated
// missing-constructor TypeError), while an explicitly allowed loopback target
// completes the TCP handshake.
void test_binding_raw_tcp_egress_gate(const char *worker_path,
                                      const char *call_path) {
    const std::string source_prefix =
        "import core from 'capsid:internal/core';"
        "export default () => ({ find() {"
        "  const socket = new core.TCP();"
        "  if (typeof socket.connect !== 'function')"
        "    return 'tcp-shape:' + typeof socket.connect + ':' +"
        "      Object.getOwnPropertyNames(Object.getPrototypeOf(socket)).join(',');"
        "  return new Promise((resolve, reject) => {"
        "    socket.onconnect = error => {"
        "      if (error) { socket.close?.(); reject(error); return; }"
        "      socket.close?.(); resolve('tcp-allowed');"
        "    };"
        "    try { socket.connect({ip:'127.0.0.1',port:";
    const std::string source_suffix =
        "}); } catch (error) {"
        "      socket.close?.(); resolve('tcp-denied:' + error.message);"
        "    }"
        "  });"
        "} });";

    const uint16_t denied_port = 9;
    const std::string denied = invoke_binding_method(
        worker_path,
        call_path,
        source_prefix + std::to_string(denied_port) + source_suffix,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        92);
    require(denied.find("result:tcp-denied:egress denied") == 0,
            "raw TCP denial did not come from the native gate: " + denied);

    uint16_t allowed_port = 0;
    const int listener = loopback_tcp_listener(&allowed_port);
    const std::string target =
        "127.0.0.1:" + std::to_string(allowed_port);
    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        source_prefix + std::to_string(allowed_port) + source_suffix,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        93,
        {},
        {},
        {target});
    close(listener);
    require(allowed == "result:tcp-allowed",
            "raw TCP rejected an explicitly allowed target: " + allowed);
}

// TLS has a separate uv_tcp_connect entry point from core.TCP. It must run
// the same resolved-address gate before the kernel connect; an allowed rule
// is distinguished by reaching the asynchronous socket/TLS callback.
void test_binding_raw_tls_egress_gate(const char *worker_path,
                                      const char *call_path) {
    const std::string source =
        "import core from 'capsid:internal/core';"
        "export default () => ({ find() {"
        " const socket = new core.TLSTcp({isServer:false,verifyPeer:false});"
        " return new Promise(resolve => {"
        "  socket.onconnect = error => { socket.close();"
        "    resolve(error ? 'tls-reached:' + error.message : 'tls-allowed');"
        "  };"
        "  try { socket.connect({ip:'127.0.0.1',port:9}); }"
        "  catch (error) { socket.close();"
        "    resolve('tls-denied:' + error.message); }"
        " });"
        "} });";

    const std::string denied = invoke_binding_method(
        worker_path, call_path, source, {}, {"network-client"},
        {"capsid:internal/core"}, 201);
    require(denied.find("result:tls-denied:egress denied") == 0,
            "raw TLS denial did not come from the native gate: " + denied);

    const std::string allowed = invoke_binding_method(
        worker_path, call_path, source, {}, {"network-client"},
        {"capsid:internal/core"}, 202, {}, {}, {"127.0.0.1:9"});
    require(allowed.find("result:tls-reached:") == 0 ||
                allowed == "result:tls-allowed",
            "raw TLS did not reach an explicitly allowed target: " + allowed);
}

// HttpClient callbacks must restore the Binding identity, and a redirect is a
// new egress decision rather than authority inherited from the first hop.
void test_binding_http_async_owner_and_redirect_gate(
    const char *worker_path,
    const char *call_path) {
    const std::string ok_response =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Connection: close\r\n\r\nok";

    uint16_t owner_port = 0;
    const int owner_listener = loopback_tcp_listener(&owner_port);
    const pid_t owner_server =
        spawn_http_responder(owner_listener, ok_response);
    const std::string owner_source =
        "import core from 'capsid:internal/core';"
        "export default () => ({ async find() {"
        " const response = await fetch('http://127.0.0.1:" +
        std::to_string(owner_port) +
        "/'); const body = await response.text();"
        " try { await core.fs.open("
        "'/tmp/capsid-binding-http-owner-missing','r');"
        "  return 'unexpected-file'; }"
        " catch (error) { return error.message.includes('fs denied')"
        "  ? 'owner-lost' : 'http-owner-ok:' + body; }"
        "} });";
    const std::string owner_result = invoke_binding_method(
        worker_path, call_path, owner_source,
        {"/tmp/capsid-binding-http-owner-missing"},
        {"network-client", "filesystem-read"},
        {"capsid:internal/core"}, 203, {}, {},
        {"127.0.0.1:" + std::to_string(owner_port)});
    finish_http_responder(owner_server, owner_listener, true);
    require(owner_result == "result:http-owner-ok:ok",
            "HTTP continuation lost Binding identity: " + owner_result);

    uint16_t target_port = 0;
    const int target_listener = loopback_tcp_listener(&target_port);
    const pid_t target_server =
        spawn_http_responder(target_listener, ok_response);
    uint16_t redirect_port = 0;
    const int redirect_listener = loopback_tcp_listener(&redirect_port);
    const std::string redirect_response =
        "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" +
        std::to_string(target_port) +
        "/next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const pid_t redirect_server =
        spawn_http_responder(redirect_listener, redirect_response);
    const std::string redirect_source =
        "export default () => ({ async find() {"
        " try { const response = await fetch('http://127.0.0.1:" +
        std::to_string(redirect_port) +
        "/'); return 'redirect-allowed:' + await response.text(); }"
        " catch (error) { return 'redirect-denied:' + error.message; }"
        "} });";
    const std::string denied = invoke_binding_method(
        worker_path, call_path, redirect_source, {}, {"network-client"},
        {"capsid:internal/core"}, 204, {}, {},
        {"127.0.0.1:" + std::to_string(redirect_port)});
    finish_http_responder(redirect_server, redirect_listener, true);
    finish_http_responder(target_server, target_listener, false);
    require(denied.find("result:redirect-denied:") == 0 &&
                denied.find("egress") != std::string::npos,
            "HTTP redirect escaped its second-hop egress gate: " + denied);

    uint16_t allowed_target_port = 0;
    const int allowed_target_listener =
        loopback_tcp_listener(&allowed_target_port);
    const pid_t allowed_target_server =
        spawn_http_responder(allowed_target_listener, ok_response);
    uint16_t allowed_redirect_port = 0;
    const int allowed_redirect_listener =
        loopback_tcp_listener(&allowed_redirect_port);
    const std::string allowed_redirect_response =
        "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" +
        std::to_string(allowed_target_port) +
        "/next\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const pid_t allowed_redirect_server =
        spawn_http_responder(
            allowed_redirect_listener, allowed_redirect_response);
    const std::string allowed_redirect_source =
        "export default () => ({ async find() {"
        " const response = await fetch('http://127.0.0.1:" +
        std::to_string(allowed_redirect_port) +
        "/'); return 'redirect-allowed:' + await response.text();"
        "} });";
    const std::string allowed = invoke_binding_method(
        worker_path, call_path, allowed_redirect_source, {},
        {"network-client"}, {"capsid:internal/core"}, 205, {}, {},
        {"127.0.0.1:" + std::to_string(allowed_redirect_port),
         "127.0.0.1:" + std::to_string(allowed_target_port)});
    finish_http_responder(
        allowed_redirect_server, allowed_redirect_listener, true);
    finish_http_responder(
        allowed_target_server, allowed_target_listener, true);
    require(allowed == "result:redirect-allowed:ok",
            "HTTP redirect rejected its explicitly allowed second hop: " +
                allowed);
}

// WebSocket is part of the grantable client surface. Its initial host and
// resolved address must both pass the egress gate, and callbacks must restore
// the immutable Binding owner before user JavaScript can operate the handle.
void test_binding_websocket_egress_and_async_owner(
    const char *worker_path,
    const char *call_path) {
    const std::string denied_source =
        "import core from 'capsid:internal/core';"
        "export default () => ({ find() {"
        " const invalid = ["
        "  () => new core.WebSocket(),"
        "  () => new core.WebSocket('http://127.0.0.1:9/', null, [], []),"
        "  () => new core.WebSocket('ws://127.0.0.1:65536/', null, [], [])"
        " ];"
        " for (const create of invalid) {"
        "  try { create(); return 'ws-invalid-accepted'; }"
        "  catch (error) { if (!(error instanceof TypeError))"
        "   return 'ws-invalid-error:' + error; }"
        " }"
        " try { new core.WebSocket('ws://127.0.0.1:9/', null, [], []);"
        "  return 'ws-unexpected'; }"
        " catch (error) { return 'ws-denied:' + error.message; }"
        "} });";
    const std::string denied = invoke_binding_method(
        worker_path, call_path, denied_source, {}, {"network-client"},
        {"capsid:internal/core"}, 206);
    require(denied.find("result:ws-denied:egress denied") == 0,
            "WebSocket denial did not come from the native gate: " + denied);

    uint16_t allowed_port = 0;
    const int listener = loopback_tcp_listener(&allowed_port);
    const pid_t server = spawn_websocket_responder(listener);
    const std::string target =
        "127.0.0.1:" + std::to_string(allowed_port);
    const std::string allowed_source =
        "import core from 'capsid:internal/core';"
        "export default () => ({ find() {"
        " return new Promise(resolve => {"
        "  const ws = new core.WebSocket('ws://" + target +
        "/', null, [], []);"
        "  ws.onerror = error => resolve('ws-error:' + String(error));"
        "  ws.onopen = () => {"
        "   try { void ws.bufferedAmount; ws.close(1000, '');"
        "    resolve('ws-allowed'); }"
        "   catch (error) { resolve('ws-owner-lost:' + error.message); }"
        "  };"
        " });"
        "} });";
    const std::string allowed = invoke_binding_method(
        worker_path, call_path, allowed_source, {}, {"network-client"},
        {"capsid:internal/core"}, 207, {}, {}, {target});
    finish_websocket_responder(server, listener);
    require(allowed == "result:ws-allowed",
            "WebSocket rejected an allowed target or lost its async owner: " +
                allowed);
}

void test_binding_raw_udp_egress_gate(const char *worker_path,
                                      const char *call_path) {
    const std::string source_prefix =
        "import core from 'capsid:internal/core';"
        "export default () => ({ find() {"
        "  const socket = new core.UDP();"
        "  try { socket.send(new Uint8Array([7]),"
        "    {ip:'127.0.0.1',port:";
    const std::string source_suffix =
        "}); socket.close(); return 'udp-allowed'; }"
        "  catch (error) { socket.close();"
        "    return 'udp-denied:' + error.message; }"
        "} });";
    const std::string denied = invoke_binding_method(
        worker_path,
        call_path,
        source_prefix + "9" + source_suffix,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        97);
    require(denied.find("result:udp-denied:egress denied") == 0,
            "raw UDP denial did not come from the native gate: " + denied);

    uint16_t allowed_port = 0;
    const int receiver = loopback_udp_receiver(&allowed_port);
    const std::string target =
        "127.0.0.1:" + std::to_string(allowed_port);
    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        source_prefix + std::to_string(allowed_port) + source_suffix,
        {},
        {"network-client"},
        {"capsid:internal/core"},
        98,
        {},
        {},
        {target});
    close(receiver);
    require(allowed == "result:udp-allowed",
            "raw UDP rejected an explicitly allowed target: " + allowed);
}

// Binding v1 P0-6: the per-origin Native Gate — a Binding writing a file
// outside its fs policy fails before the syscall, on the raw core.fs
// path (not only through a capsid facade).
void test_binding_fs_native_gate(const char *worker_path,
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
        "import core from 'capsid:internal/core';"
        "export default ({ config, secrets, log }) => {"
        "  return { find() {"
        "    return core.fs.open('/tmp/capsid-binding-fs-probe', 'w');"
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
            "fs-gate binding worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 80;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "fs-gate response frames stopped before the terminal");
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 80) {
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
    require(ended, "fs-gate binding request never terminated");
    require(body.find("fs denied") != std::string::npos ||
                body.find("binding call failed") != std::string::npos,
            "fs-gate denial carries no failure text: " + body);
    finish_worker(fd, pid, true);
}

// FSWatch is a separately declared syscall profile and a path-bearing read
// operation. Because the OS sandbox is a process-wide union, both checks must
// also happen in the active Binding policy before uv_fs_event_start.
void test_binding_fswatch_profile_and_path_gate(const char *worker_path,
                                                const char *call_path) {
    char path[] = "/tmp/capsid-binding-fswatch-XXXXXX";
    const int fixture = mkstemp(path);
    require(fixture >= 0, "could not create FSWatch fixture");
    close(fixture);
    const std::string source =
        std::string("import core from 'capsid:internal/core';") +
        "export default () => ({ find() { try {" +
        " const watcher = core.watch('" + path + "', () => {});" +
        " watcher.close(); return 'watch-allowed';" +
        " } catch (error) { return 'watch-denied:' + error.message; }" +
        "} });";

    const std::string no_profile = invoke_binding_method(
        worker_path,
        call_path,
        source,
        {path},
        {"filesystem-read"},
        {"capsid:internal/core"},
        101);
    require(
        no_profile ==
            "result:watch-denied:CAPSID_FSWATCH_PROFILE_DENIED",
        "FSWatch escaped its sandbox profile gate: " + no_profile);

    const std::string no_path = invoke_binding_method(
        worker_path,
        call_path,
        source,
        {},
        {"filesystem-read", "filesystem-watch"},
        {"capsid:internal/core"},
        102);
    require(no_path.find(
                "result:watch-denied:fs denied by binding policy:") == 0,
            "FSWatch escaped its path gate: " + no_path);

    const std::string allowed = invoke_binding_method(
        worker_path,
        call_path,
        source,
        {path},
        {"filesystem-read", "filesystem-watch"},
        {"capsid:internal/core"},
        103);
    unlink(path);
    require(allowed == "result:watch-allowed",
            "FSWatch rejected an exact profile/path grant: " + allowed);
}

// Binding v1 §5.1: async continuations propagate the Binding identity —
// a timer callback performing an authorized fs read still passes the
// Native gate (the dispatch window alone would have closed).
void test_binding_async_identity(const char *worker_path,
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
        "import core from 'capsid:internal/core';"
        "export default ({ config, secrets, log }) => {"
        "  return { find() {"
        "    return new Promise((resolve, reject) => {"
        "      setTimeout(() => {"
        "        core.fs.open('/etc/capsid/mongo/ca.pem', 'r').then("
        "          () => resolve('unexpected-open'),"
        "          (e) => reject(e)"
        "        );"
        "      }, 0);"
        "    });"
        "  } };"
        "};",
        {"/etc/capsid/mongo"},
        {"network-client", "filesystem-read"});
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
            "async-identity binding worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 81;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "async-identity response frames stopped before the terminal");
        if (frame.type == capsid::protocol::kError &&
            frame.request_id == 81) {
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
    require(ended, "async-identity binding request never terminated");
    // The gate must have PASSED the authorized path inside the timer
    // callback: the failure text is the missing file, never a policy
    // denial.
    require(body.find("fs denied") == std::string::npos,
            "async continuation lost the binding identity: " + body);
    finish_worker(fd, pid, true);
}

// Binding v1 §7.9: the wasi profile runs a real WebAssembly workload
// inside the Binding Runtime — the positive counterpart to the
// not-implemented gate of earlier iterations.
void test_binding_wasi_workload(const char *worker_path,
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
        wasi_fd_close_binding_source(
            "{version:'wasi_snapshot_preview1',"
            "preopens:{'/guest':'/tmp'}}"),
        {},
        {"filesystem-write", "wasi"},
        {"capsid:internal/core", "capsid:wasi"},
        {"/tmp"});
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
            "wasi binding worker did not report READY");

    capsid::protocol::Frame head;
    head.type = capsid::protocol::kRequestHead;
    head.flags = capsid::protocol::kFlagRequestEnd;
    head.request_id = 82;
    append_string16(&head.payload, "GET");
    append_string32(&head.payload, "https://example.test/");
    capsid::protocol::append_u16(&head.payload, 0);
    send_frame(fd, head);

    std::string body;
    bool ended = false;
    for (int i = 0; i < 64; ++i) {
        require(
            read_frame(fd, &parser, &frame, 5000) == ReadResult::kFrame,
            "wasi response frames stopped before ResponseEnd");
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
    require(ended, "wasi binding response never ended");
    require(body == "result:wasi-allowed",
            "wasi workload returned the wrong body: " + body);
    finish_worker(fd, pid, true);
}

// Binding v1 §3.3: every module the manifest schema can grant must
// actually exist in this restricted build — the grantable set and the
// build must not drift.
void test_binding_grantable_modules_exist(const char *worker_path,
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
        "import assert_mod from 'capsid:assert';"
        "import getopts_mod from 'capsid:getopts';"
        "import * as hashing_mod from 'capsid:hashing';"
        "import core from 'capsid:internal/core';"
        "import internal_path from 'capsid:internal/path';"
        "import ipaddr_mod from 'capsid:ipaddr';"
        "import path_mod from 'capsid:path';"
        "import readline_mod from 'capsid:readline';"
        "import * as sqlite_mod from 'capsid:sqlite';"
        "import * as utils_mod from 'capsid:utils';"
        "import uuid_mod from 'capsid:uuid';"
        "import wasi_mod from 'capsid:wasi';"
        "const seen = [assert_mod, getopts_mod, hashing_mod, core,"
        "  internal_path, ipaddr_mod, path_mod,"
        "  readline_mod, sqlite_mod, utils_mod, uuid_mod, wasi_mod];"
        "export default ({ config, secrets, log }) => {"
        "  return { find() { return 'modules:' + seen.length; } };"
        "};",
        {},
        {"network-client", "sqlite", "wasi"},
        {"capsid:assert", "capsid:getopts", "capsid:hashing",
         "capsid:internal/core", "capsid:internal/path", "capsid:ipaddr",
         "capsid:path", "capsid:readline",
         "capsid:sqlite", "capsid:utils", "capsid:uuid", "capsid:wasi"});
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
            "grantable-modules binding worker did not report READY");
    finish_worker(fd, pid, true);
}

// Binding v1 §3.3 negative module matrix: private tjs:* names and every
// permanently forbidden Capsid module fail Binding package evaluation, even
// though several of them are present in the worker for other Binding
// grants. Static imports make warm-up fail before READY with the loader's
// authorization diagnostic.
void test_binding_forbidden_imports_fail(const char *worker_path,
                                         const char *p0_path) {
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import fs from 'tjs:fs';"
        "export default () => ({ find() { return fs; } });",
        "module is not authorized for this binding: tjs:fs");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import sqlite from 'tjs:sqlite';"
        "export default () => ({ find() { return sqlite; } });",
        "module is not authorized for this binding: tjs:sqlite");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import core from 'tjs:internal/core';"
        "export default () => ({ find() { return core; } });",
        "module is not authorized for this binding: tjs:internal/core");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import ffi from 'capsid:ffi';"
        "export default () => ({ find() { return ffi; } });",
        "module is not authorized for this binding: capsid:ffi");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import socket from 'capsid:posix-socket';"
        "export default () => ({ find() { return socket; } });",
        "module is not authorized for this binding: capsid:posix-socket");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import process from 'capsid:process';"
        "export default () => ({ find() { return process; } });",
        "module is not authorized for this binding: capsid:process");
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import server from 'capsid:http-server';"
        "export default () => ({ find() { return server; } });",
        "module is not authorized for this binding: capsid:http-server");
    // Known and grantable, but not listed in this Binding's manifest:
    // listing one module never implicitly authorizes another.
    expect_binding_startup_failure(
        worker_path, p0_path,
        "import sqlite from 'capsid:sqlite';"
        "export default () => ({ find() { return sqlite; } });",
        "module is not authorized for this binding: capsid:sqlite");
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
    if (argc != 7) {
        fail("expected worker path, p0 fixture, binding-import fixture "
             "binding-call fixture, binding-owner fixture and "
             "binding-lifecycle fixture");
    }
    test_zero_binding_ready_baseline(argv[1], argv[2]);
    test_undeclared_binding_import_fails(argv[1], argv[3]);
    test_load_binding_after_bundle_is_rejected(argv[1], argv[2]);
    test_binding_ready_proof(argv[1], argv[2]);
    test_binding_factory_failures(argv[1], argv[2]);
    test_binding_end_to_end_call(argv[1], argv[4]);
    test_binding_init_object_contract(argv[1], argv[4]);
    test_binding_method_table_is_frozen(argv[1], argv[4]);
    test_binding_log_envelope_and_redaction(argv[1], argv[4]);
    test_binding_isolation_globals_modules_and_handles(argv[1], argv[5]);
    test_binding_rpc_lifecycle(argv[1], argv[6]);
    test_binding_sync_loop_interrupts(argv[1], argv[4]);
    test_binding_core_is_client_only(argv[1], argv[4]);
    test_binding_readline_has_no_ambient_stdio(argv[1], argv[4]);
    test_binding_open_file_has_no_descriptor_surface(argv[1], argv[4]);
    test_binding_dns_egress_gate(argv[1], argv[4]);
    test_binding_wasi_preopen_is_policy_gated(argv[1], argv[4]);
    test_binding_wasi_allowed_preopen_runs(argv[1], argv[4]);
    test_binding_wasi_stdio_is_policy_gated(argv[1], argv[4]);
    test_binding_sqlite_paths_and_extensions_are_gated(argv[1], argv[4]);
    test_binding_egress_denial(argv[1], argv[4]);
    test_binding_raw_tcp_egress_gate(argv[1], argv[4]);
    test_binding_raw_tls_egress_gate(argv[1], argv[4]);
    test_binding_http_async_owner_and_redirect_gate(argv[1], argv[4]);
    test_binding_websocket_egress_and_async_owner(argv[1], argv[4]);
    test_binding_raw_udp_egress_gate(argv[1], argv[4]);
    test_binding_fs_native_gate(argv[1], argv[4]);
    test_binding_fswatch_profile_and_path_gate(argv[1], argv[4]);
    test_binding_async_identity(argv[1], argv[4]);
    test_binding_wasi_workload(argv[1], argv[4]);
    test_binding_forbidden_imports_fail(argv[1], argv[2]);
    test_binding_grantable_modules_exist(argv[1], argv[4]);
    test_load_binding_abi_validation();
    return 0;
}
