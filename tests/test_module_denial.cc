#include "capsid/runtime.h"

#include <poll.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string make_bundle(const char *specifier) {
    return std::string("import '") + specifier +
           "';\nexport default { fetch() { return new Response(); } };\n";
}

void expect_import_denied(capsid_worker *worker,
                          const char *specifier,
                          bool explicitly_forbidden,
                          bool explicitly_unauthorized) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const bool forbidden = explicitly_forbidden ||
        std::string(specifier).find("tjs:") == 0 ||
        std::string(
            specifier).find("tjs:internal/") == 0 ||
        std::string(specifier).find("node:") == 0 ||
        std::string(specifier).find("file:") == 0 ||
        std::string(specifier).find("http:") == 0 ||
        std::string(specifier).find("https:") == 0;
    const std::string expected =
        std::string(forbidden
                        ? "module is forbidden: "
                        : explicitly_unauthorized
                              ? "module is not authorized: "
                              : "module is unavailable: ") +
        specifier;

    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK &&
            flush != CAPSID_CLOSED) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }

        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string message(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                if (message.find(expected) == std::string::npos) {
                    fail(std::string("unexpected module denial error: ") +
                         message);
                }
                return;
            }
            if (event.type == CAPSID_EVENT_READY) {
                fail(std::string("forbidden module was resolved: ") +
                     specifier);
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited without reporting the module denial");
            }
        } else if (result == CAPSID_CLOSED) {
            fail(
                std::string(
                    "worker closed before module denial: ") +
                specifier);
        } else if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for module denial");
        }

        const int fd = capsid_worker_fd(worker);
        if (fd >= 0) {
            struct pollfd descriptor = {};
            descriptor.fd = fd;
            descriptor.events =
                POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            poll(&descriptor, 1, 50);
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fail("expected worker path and one or more denied module specifiers");
    }

    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const std::string forbidden_prefix = "forbidden=";
        const std::string unavailable_prefix = "unavailable=";
        const std::string unauthorized_prefix = "unauthorized=";
        bool explicitly_forbidden = false;
        bool explicitly_unavailable = false;
        bool explicitly_unauthorized = false;
        std::string specifier = argument;
        if (argument.find(forbidden_prefix) == 0) {
            explicitly_forbidden = true;
            specifier = argument.substr(forbidden_prefix.size());
        } else if (argument.find(unavailable_prefix) == 0) {
            explicitly_unavailable = true;
            specifier = argument.substr(unavailable_prefix.size());
        } else if (argument.find(unauthorized_prefix) == 0) {
            explicitly_unauthorized = true;
            specifier = argument.substr(unauthorized_prefix.size());
        }
        if (specifier.empty()) {
            fail("empty module specifier in denial matrix");
        }

        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[1];
        capsid_capability_policy capability;
        capsid_capability_policy_init(&capability);
        const char *allowed_module = specifier.c_str();
        if (explicitly_unavailable) {
            /*
             * A known module omitted from the allow-list is correctly
             * classified as "not authorized". Grant it here so this manifest
             * matrix reaches the independent build-availability decision and
             * proves that known_but_not_built entries remain unavailable.
             */
            capability.application_identity = "module-denial-matrix";
            capability.allowed_modules = &allowed_module;
            capability.allowed_module_count = 1;
            config.capability_policy = &capability;
        }

        capsid_worker *worker = NULL;
        require_result(capsid_worker_spawn(&config, &worker), "spawn worker");

        const std::string bundle = make_bundle(specifier.c_str());
        require_result(
            capsid_worker_load_bundle(
                worker,
                reinterpret_cast<const uint8_t *>(bundle.data()),
                bundle.size()),
            "load bundle");

        expect_import_denied(
            worker,
            specifier.c_str(),
            explicitly_forbidden,
            explicitly_unauthorized);
        capsid_worker_destroy(worker);
    }
    return 0;
}
