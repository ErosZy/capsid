// Protocol options descriptor: two-way size negotiation and spawn_ex
// gating (v0.3.0 remediation T01).
//
// Contract: docs/v0.3.0-implementation-design.md §2.2 — the published
// capsid_worker_config layout is frozen at ABI v7; protocol selection
// lives in a separately versioned descriptor with explicit caller-size
// negotiation, validated before any OS side effect.
#include "capsid/runtime.h"
#include "win32_compat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char *kBundle =
    "export default { async fetch(r) {"
    "  return new Response('x', { status: 200 });"
    "}};\n";

void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

void require_ok(capsid_result result, const char *what) {
    if (result != CAPSID_OK) {
        fail(std::string(what) + ": " + capsid_result_string(result));
    }
}

void require_arg(capsid_result result, const char *what) {
    if (result != CAPSID_INVALID_ARGUMENT) {
        fail(std::string(what) + " must be INVALID_ARGUMENT, got: " +
             capsid_result_string(result));
    }
}

void require_block(capsid_result result, const char *what) {
    if (result != CAPSID_WOULD_BLOCK) {
        fail(std::string(what) + " must be WOULD_BLOCK, got: " +
             capsid_result_string(result));
    }
}

// Sentinel frame around an undersized caller buffer: the library must
// never write beyond caller_size.
struct Guarded {
    uint64_t before = UINT64_C(0x1122334455667788);
    capsid_protocol_options options;
    uint64_t after = UINT64_C(0x8877665544332211);
};

struct Pump {
    capsid_worker *worker = nullptr;
    bool ready = false;

    void step(const char *what) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string(what) + " flush: " +
                 capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                ready = true;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail(std::string(what) + ": worker error/exit");
            }
        } else if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string(what) + " event: " +
                 capsid_result_string(result));
        }
    }

    void wait_ready(const char *what) {
        for (int i = 0; i < 200 && !ready; ++i) {
            step(what);
            if (ready) {
                return;
            }
            capsid_pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN;
            capsid::win32::capsid_poll(&descriptor, 1, 25);
        }
        if (!ready) {
            fail(std::string(what) + ": READY timeout");
        }
    }
};

void test_init_size_negotiation() {
    // Full-size buffer: defaults land.
    capsid_protocol_options full;
    std::memset(&full, 0xA5, sizeof(full));
    require_ok(
        capsid_protocol_options_init(&full, sizeof(full)), "init full");
    if (full.struct_size != sizeof(full) ||
        full.descriptor_version != CAPSID_PROTOCOL_DESCRIPTOR_VERSION ||
        full.wire_version != 3 || full.reserved16 != 0 ||
        full.capabilities != 0 || full.max_channels != 0) {
        fail("full-size init defaults");
    }

    // Smaller future-compatible buffer: only the covered prefix is
    // written, struct_size records the caller's size, and the tail of
    // the caller's buffer stays untouched (sentinels intact).
    Guarded guarded;
    std::memset(&guarded.options, 0xA5, sizeof(guarded.options));
    const uint32_t small =
        offsetof(capsid_protocol_options, capabilities) +
        sizeof(uint32_t);
    require_ok(
        capsid_protocol_options_init(&guarded.options, small),
        "init small");
    if (guarded.before != UINT64_C(0x1122334455667788) ||
        guarded.after != UINT64_C(0x8877665544332211)) {
        fail("small init overran the caller buffer");
    }
    if (guarded.options.struct_size != small) {
        fail("small init must record caller_size");
    }
    // Bytes past the covered prefix must be untouched by init.
    const unsigned char *raw =
        reinterpret_cast<const unsigned char *>(&guarded.options);
    for (size_t i = small; i < sizeof(guarded.options); ++i) {
        if (raw[i] != 0xA5) {
            fail("small init wrote beyond caller_size");
        }
    }

    // Too small to hold the fixed prefix: nothing is written.
    Guarded tiny;
    std::memset(&tiny.options, 0xA5, sizeof(tiny.options));
    require_arg(
        capsid_protocol_options_init(&tiny.options, 4), "init tiny");
    if (tiny.before != UINT64_C(0x1122334455667788) ||
        tiny.after != UINT64_C(0x8877665544332211)) {
        fail("tiny init overran");
    }
    const unsigned char *tiny_raw =
        reinterpret_cast<const unsigned char *>(&tiny.options);
    for (size_t i = 0; i < sizeof(tiny.options); ++i) {
        if (tiny_raw[i] != 0xA5) {
            fail("rejected init must write nothing");
        }
    }

    require_arg(capsid_protocol_options_init(nullptr, sizeof(full)),
                "init null");
}

void test_spawn_ex_rejections(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;

    capsid_protocol_options options;
    require_ok(
        capsid_protocol_options_init(&options, sizeof(options)),
        "init for rejections");

    // Unknown wire version.
    options.wire_version = 5;
    {
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(0x1);
        require_arg(
            capsid_worker_spawn_ex(&config, &options, &worker),
            "unknown wire version");
        if (worker != nullptr) {
            fail("rejected spawn must NULL the out param");
        }
    }

    // Unknown capability bit.
    require_ok(
        capsid_protocol_options_init(&options, sizeof(options)),
        "reinit");
    options.capabilities = 1u << 30;
    {
        capsid_worker *worker = nullptr;
        require_arg(
            capsid_worker_spawn_ex(&config, &options, &worker),
            "unknown capability bit");
    }

    // Reserved words nonzero.
    require_ok(
        capsid_protocol_options_init(&options, sizeof(options)),
        "reinit");
    options.reserved16 = 1;
    {
        capsid_worker *worker = nullptr;
        require_arg(
            capsid_worker_spawn_ex(&config, &options, &worker),
            "reserved16 nonzero");
    }

    // Unknown descriptor version.
    require_ok(
        capsid_protocol_options_init(&options, sizeof(options)),
        "reinit");
    options.descriptor_version = 99;
    {
        capsid_worker *worker = nullptr;
        require_arg(
            capsid_worker_spawn_ex(&config, &options, &worker),
            "unknown descriptor version");
    }

    // NULL pointers.
    require_arg(capsid_worker_spawn_ex(nullptr, &options, nullptr),
                "null config");
    {
        capsid_worker *worker = nullptr;
        require_arg(
            capsid_worker_spawn_ex(nullptr, &options, &worker),
            "null config with out");
    }
}

void test_info_lifecycle(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;

    capsid_protocol_options v4;
    require_ok(
        capsid_protocol_options_init(&v4, sizeof(v4)), "init v4");
    v4.wire_version = 4;

    capsid_worker *worker = nullptr;
    require_ok(
        capsid_worker_spawn_ex(&config, &v4, &worker), "spawn v4");
    Pump pump;
    pump.worker = worker;

    // Before READY: WOULD_BLOCK, and the caller buffer is untouched.
    capsid_protocol_info info;
    std::memset(&info, 0xA5, sizeof(info));
    require_block(
        capsid_worker_get_protocol_info(worker, &info, sizeof(info)),
        "info before READY");
    if (info.struct_size != 0xA5A5A5A5u) {
        fail("pre-READY info must not write the caller buffer");
    }

    require_ok(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kBundle),
            std::strlen(kBundle)),
        "load bundle");
    pump.wait_ready("v4 ready");

    // After READY: negotiated state, exact values for the carriage
    // slice (worker announces zero capability bits).
    require_ok(
        capsid_worker_get_protocol_info(worker, &info, sizeof(info)),
        "info after READY");
    if (info.wire_version != 4 || info.host_capabilities != 0 ||
        info.worker_capabilities != 0 ||
        info.effective_capabilities != 0) {
        fail("v4 protocol info values");
    }

    // Undersized info buffer: covered prefix only; a too-small buffer
    // is an argument error that writes nothing.
    capsid_protocol_info small;
    std::memset(&small, 0xA5, sizeof(small));
    const uint32_t prefix =
        offsetof(capsid_protocol_info, host_capabilities) +
        sizeof(uint32_t);
    require_ok(
        capsid_worker_get_protocol_info(worker, &small, prefix),
        "info small");
    if (small.struct_size != prefix || small.wire_version != 4) {
        fail("small info contents");
    }
    const uint8_t *info_bytes = reinterpret_cast<const uint8_t *>(&small);
    for (size_t i = prefix; i < sizeof(small); ++i) {
        if (info_bytes[i] != 0xA5) fail("info wrote beyond caller_size");
    }
    capsid_protocol_info tiny;
    std::memset(&tiny, 0xA5, sizeof(tiny));
    require_arg(
        capsid_worker_get_protocol_info(worker, &tiny, 8), "info tiny");
    if (tiny.struct_size != 0xA5A5A5A5u) {
        fail("rejected info must write nothing");
    }
    require_arg(
        capsid_worker_get_protocol_info(nullptr, &tiny, sizeof(tiny)),
        "info null worker");

    capsid_worker_destroy(worker);
}

void test_null_options_is_v3(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker *worker = nullptr;
    require_ok(
        capsid_worker_spawn_ex(&config, nullptr, &worker),
        "null options spawn");
    Pump pump;
    pump.worker = worker;
    require_ok(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(kBundle),
            std::strlen(kBundle)),
        "load bundle");
    pump.wait_ready("v3 ready");
    // v3 connection: info reports the frozen v3 shape.
    capsid_protocol_info info;
    require_ok(
        capsid_worker_get_protocol_info(worker, &info, sizeof(info)),
        "info v3");
    if (info.wire_version != 3) {
        fail("null options must be v3");
    }
    capsid_worker_destroy(worker);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <capsid-worker>\n", argv[0]);
        return 2;
    }
    const char *worker_path = argv[1];
    test_init_size_negotiation();
    test_spawn_ex_rejections(worker_path);
    test_info_lifecycle(worker_path);
    test_null_options_is_v3(worker_path);
    std::printf("protocol options size negotiation: PASS\n");
    return 0;
}
