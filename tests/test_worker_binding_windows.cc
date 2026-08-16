// Windows Binding v1 smoke: a real capsid-worker process loads one Binding
// through the public C ABI and serves a User request that calls the
// synthetic capsid:binding facade. This is the Windows complement to the
// POSIX-only worker_zero_binding_regression suite: it proves LOAD_BINDING,
// Binding Runtime creation, the facade bridge and response credit all work
// on the Windows worker transport.

#include "capsid/runtime.h"
#include "win32_compat.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

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

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void wait_for_ready(capsid_worker *worker) {
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
                     std::string(
                         reinterpret_cast<const char *>(event.payload.data),
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
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

void run_binding_request(capsid_worker *worker, std::string *body) {
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", "https://example.test/", NULL, 0),
        "begin bodyless request");

    bool response_end = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!response_end) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("request flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                body->append(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id, event.payload.size),
                    "grant response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                response_end = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("request error: ") +
                     std::string(
                         reinterpret_cast<const char *>(event.payload.data),
                         event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before the response completed");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("request event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the Binding response");
        }

        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("usage: test-worker-binding-windows <capsid-worker> "
             "<binding-call-fixture>");
    }
    const std::string worker_path = argv[1];
    const std::string app_bundle = read_file(argv[2]);

    // Host-trusted package source: a trivial factory that proves the
    // Binding Runtime can evaluate a module and serve a User call. It also
    // stamps this Binding's own globalThis and the `capsid:getopts` module
    // instance, so the smoke covers per-Binding global/module ownership
    // bookkeeping on Windows.
    const std::string binding_source =
        "import getopts from 'capsid:getopts';"
        "export default function createBinding({ config, secrets, log }) {"
        "  getopts.__capsidWindowsMarker = 'windows-module-mark';"
        "  globalThis.__capsidWindowsProbe = 'windows-global';"
        "  return {"
        "    async find(input) {"
        "      return 'binding:' + JSON.stringify(input) + ':' +"
        "             (config && config.mode ? config.mode : 'default') + ':' +"
        "             globalThis.__capsidWindowsProbe + ':' +"
        "             getopts.__capsidWindowsMarker;"
        "    }"
        "  };"
        "}";

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path.c_str();

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    require(worker != NULL, "spawn returned a null worker");

    const char *modules[] = { "capsid:utils", "capsid:getopts" };
    capsid_binding_policy policy = {};
    policy.modules = modules;
    policy.module_count = 2;
    policy.net_rules = NULL;
    policy.net_rule_count = 0;
    policy.fs_read = NULL;
    policy.fs_read_count = 0;
    policy.fs_write = NULL;
    policy.fs_write_count = 0;
    policy.env = NULL;
    policy.env_count = 0;
    policy.stdio = NULL;
    policy.stdio_count = 0;

    capsid_sandbox_requirements sandbox = {};
    sandbox.profiles = NULL;
    sandbox.profile_count = 0;

    const char config_json[] = "{\"mode\":\"windows-smoke\"}";
    capsid_binding_descriptor binding = {};
    binding.struct_size = sizeof(binding);
    binding.version = CAPSID_BINDING_DESCRIPTOR_VERSION;
    binding.binding_name = "mongo";
    binding.source.data =
        reinterpret_cast<const uint8_t *>(binding_source.data());
    binding.source.size = binding_source.size();
    binding.config_json.data =
        reinterpret_cast<const uint8_t *>(config_json);
    binding.config_json.size = sizeof(config_json) - 1;
    binding.secrets = NULL;
    binding.secret_count = 0;
    binding.policy = &policy;
    binding.sandbox = &sandbox;

    require_result(capsid_worker_load_binding(worker, &binding),
                   "load binding");
    require_result(
        capsid_worker_load_bundle_named(
            worker,
            reinterpret_cast<const uint8_t *>(app_bundle.data()),
            app_bundle.size(),
            "file:///capsid/test/binding-call.js"),
        "load bundle");

    wait_for_ready(worker);

    std::string body;
    run_binding_request(worker, &body);
    require(body.find("result:binding:{\"collection\":\"users\"}:"
                      "windows-smoke:windows-global:windows-module-mark") !=
                std::string::npos,
            "Binding facade response did not carry the Binding result: " +
                body);

    require_result(capsid_worker_shutdown(worker), "shutdown worker");
    capsid_worker_destroy(worker);
    std::cout << "binding windows smoke: ok" << std::endl;
    return 0;
}
