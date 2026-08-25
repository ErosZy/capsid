// End-to-end differential gate for the bytecode AOT rewriter
// (docs/bytecode-aot-rewriter.md): for every fixture spec the
// compiler — which runs the rewriter on its output — produces a .qjsb
// bundle, then two workers are driven through the identical request:
//   - source worker:    capsid_worker_load_bundle_named(source)
//   - bytecode worker:  capsid_worker_load_trusted_bytecode_named(bundle)
// and the (status, body) pairs must match byte for byte.
//
// Specs are "mode:path" arguments:
//   normal:<path>   self-contained fixture, deterministic on GET /sync
//   failload:<path> the bundle must fail closed at load (kError before
//                   READY) in BOTH workers, with identical error text
//
// No binding mode: the frozen compiler rejects every non-entry module
// import ("module is unavailable"), so an app importing capsid:binding/*
// cannot be compiled by the CLI at all — the binding path never sees the
// rewriter (it is host-side runtime code) and is out of scope here.

#include "capsid/runtime.h"

#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <arpa/inet.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/wait.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char* operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open file: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> read_bytes(const char* path) {
    const std::string text = read_file(path);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string run_command(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    std::string command;
    for (const std::string& argument : argv) {
        if (!command.empty()) {
            command += " ";
        }
        command += "\"";
        for (const char c : argument) {
            command += c == '"' ? "\\\"" : std::string(1, c);
        }
        command += "\"";
    }
    command = "cmd /c \"" + command + " 2>&1\"";
    FILE* stream = _popen(command.c_str(), "rb");
    if (stream == nullptr) {
        fail("cannot spawn command");
    }
    std::string output;
    char buffer[4096];
    for (;;) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), stream);
        if (count == 0) {
            break;
        }
        output.append(buffer, count);
    }
    const int status = _pclose(stream);
    if (status != 0) {
        fail("command failed (" + argv[0] + "): " + output);
    }
    return output;
#else
    std::vector<char*> args;
    for (const std::string& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);

    int pipes[2];
    if (pipe(pipes) != 0) {
        fail("cannot create pipe");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        fail("cannot fork");
    }
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execv(args[0], args.data());
        _exit(127);
    }
    close(pipes[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipes[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("command failed (" + argv[0] + "): " + output);
    }
    return output;
#endif
}

// ---- worker harness (same contract as the RED round-trip test) ----

void wait_for_ready(capsid_worker* worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

// The worker must fail closed at bundle load: CAPSID_EVENT_ERROR (kError)
// before READY. Returns the error payload.
std::string wait_for_load_failure(capsid_worker* worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_ERROR) {
                return std::string(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
            }
            if (event.type == CAPSID_EVENT_READY) {
                fail("expected a load failure, worker became READY");
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before reporting a load error");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the load failure");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

struct Response {
    int status = 0;
    std::string body;
};

Response run_request(capsid_worker* worker) {
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", "https://example.test/sync", NULL, 0),
        "begin bodyless request");
    Response response;
    bool received_head = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("request flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != 1) {
                    fail("unexpected response head");
                }
                response.status = event.status;
                received_head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != 1) {
                    fail("response body arrived before its head");
                }
                response.body.append(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<std::uint32_t>(event.payload.size)),
                    "replenish response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != 1) {
                    fail("unexpected response end");
                }
                return response;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker request error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("request event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the response");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

capsid_worker* spawn_worker(const char* worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker* worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    return worker;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fail("expected worker path, compiler tool and fixture specs");
    }
    const char* worker_path = argv[1];
    const char* compiler_tool = argv[2];

    std::string work_dir;
#if defined(_WIN32)
    work_dir = "capsid-rewrite-diff";
    std::error_code ignored;
    std::filesystem::remove_all(work_dir, ignored);
    std::filesystem::create_directories(work_dir);
#else
    work_dir = "/tmp/capsid-rewrite-diff";
    const std::string command =
        std::string("rm -rf ") + work_dir + " && mkdir -p " + work_dir;
    if (system(command.c_str()) != 0) {
        fail("cannot prepare work dir");
    }
#endif

    for (int index = 3; index < argc; ++index) {
        const std::string spec = argv[index];
        const std::string::size_type separator = spec.find(':');
        if (separator == std::string::npos) {
            fail("fixture spec must be mode:path: " + spec);
        }
        const std::string mode = spec.substr(0, separator);
        const std::string fixture_path = spec.substr(separator + 1);
        const std::string::size_type slash = fixture_path.find_last_of("/\\");
        const std::string basename =
            slash == std::string::npos ? fixture_path
                                       : fixture_path.substr(slash + 1);
        const std::string source_name = "file:///app/" + basename;
        const bool expect_load_failure = mode == "failload";
        if (!expect_load_failure && mode != "normal") {
            fail("unknown fixture mode: " + mode);
        }

        // 1. Compile with the frozen CLI (the rewriter runs inside).
        const std::string bytecode_out = work_dir + "/" + basename + ".qjsb";
        const std::string attestation_out = work_dir + "/" + basename + ".json";
        const std::string message_out = work_dir + "/" + basename + ".bin";
        run_command({
            compiler_tool,
            "--source", fixture_path,
            "--source-name", source_name,
            "--application", "orders",
            "--version", "2026-08-03-001",
            "--key-id", "test-key-1",
            "--bytecode-out", bytecode_out,
            "--attestation-out", attestation_out,
            "--signing-message-out", message_out,
        });
        const std::vector<std::uint8_t> bytecode =
            read_bytes(bytecode_out.c_str());
        if (bytecode.empty()) {
            fail("compiler produced an empty bundle for " + basename);
        }

        const std::string label = basename;
        if (expect_load_failure) {
            // 2. Both load paths must fail closed with identical errors.
            const std::string source = read_file(fixture_path.c_str());
            std::string source_error;
            {
                capsid_worker* worker = spawn_worker(worker_path);
                require_result(
                    capsid_worker_load_bundle_named(
                        worker,
                        reinterpret_cast<const std::uint8_t*>(source.data()),
                        source.size(), source_name.c_str()),
                    "load source bundle");
                source_error = wait_for_load_failure(worker);
                capsid_worker_destroy(worker);
            }
            std::string bytecode_error;
            {
                capsid_worker* worker = spawn_worker(worker_path);
                require_result(
                    capsid_worker_load_trusted_bytecode_named(
                        worker, bytecode.data(), bytecode.size(),
                        source_name.c_str()),
                    "load trusted bytecode");
                bytecode_error = wait_for_load_failure(worker);
                capsid_worker_destroy(worker);
            }
            if (source_error != bytecode_error) {
                fail(label + ": load failure text differs ('" + source_error +
                     "' vs '" + bytecode_error + "')");
            }
            std::cout << label << ": ok (load failed closed)" << std::endl;
            continue;
        }

        // 2. Source worker.
        const std::string source = read_file(fixture_path.c_str());
        Response source_response;
        {
            capsid_worker* worker = spawn_worker(worker_path);
            require_result(
                capsid_worker_load_bundle_named(
                    worker,
                    reinterpret_cast<const std::uint8_t*>(source.data()),
                    source.size(), source_name.c_str()),
                "load source bundle");
            wait_for_ready(worker);
            source_response = run_request(worker);
            capsid_worker_destroy(worker);
        }

        // 3. Optimized-bytecode worker.
        Response bytecode_response;
        {
            capsid_worker* worker = spawn_worker(worker_path);
            require_result(
                capsid_worker_load_trusted_bytecode_named(
                    worker, bytecode.data(), bytecode.size(),
                    source_name.c_str()),
                "load trusted bytecode");
            wait_for_ready(worker);
            bytecode_response = run_request(worker);
            capsid_worker_destroy(worker);
        }

        if (source_response.status != bytecode_response.status) {
            fail(label + ": status differs (" +
                 std::to_string(source_response.status) + " vs " +
                 std::to_string(bytecode_response.status) + ")");
        }
        if (source_response.body != bytecode_response.body) {
            fail(label + ": response body differs (" +
                 std::to_string(source_response.body.size()) + " vs " +
                 std::to_string(bytecode_response.body.size()) +
                 " bytes)\n  source: " + source_response.body +
                 "\n  bytecode: " + bytecode_response.body);
        }
        std::cout << label << ": ok (" << bytecode_response.status << ", "
                  << bytecode_response.body.size() << " bytes)" << std::endl;
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
