#include "sandbox.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#if !defined(CAPSID_SANITIZER_SANDBOX)
#error This control requires the explicitly selected sanitizer sandbox profile.
#endif

__attribute__((noinline)) static void *leak_from_terminated_thread(void *) {
    void *allocation = std::malloc(128);
    if (!allocation) std::abort();
    std::memset(allocation, 7, 128);
    asm volatile("" : : "r"(allocation) : "memory");
    return nullptr;
}

int main(int argc, char **argv) {
    if (argc != 2 || (std::strcmp(argv[1], "clean") && std::strcmp(argv[1], "leak"))) return 2;
    const bool leak = std::strcmp(argv[1], "leak") == 0;
    if (leak) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, leak_from_terminated_thread, nullptr) ||
            pthread_join(thread, nullptr)) return 3;
    }
    capsid::SandboxConfig config;
    config.strict = true;
    config.required_features = CAPSID_SANDBOX_FEATURE_RLIMITS |
        CAPSID_SANDBOX_FEATURE_NO_NEW_PRIVS | CAPSID_SANDBOX_FEATURE_LANDLOCK |
        CAPSID_SANDBOX_FEATURE_SECCOMP;
    uint32_t features=0, abi=0, mode=0;
    std::string error;
    if (!capsid::apply_sandbox(config, &features, &abi, &mode, &error)) {
        std::fprintf(stderr, "sandbox control: %s\n", error.c_str());
        return 4;
    }
    if ((features & config.required_features) != config.required_features || !abi ||
        mode != capsid::kSeccompModeFilter) return 5;
    std::fprintf(stderr, "SANITIZER_LEAK_CONTROL features=%u landlock=%u seccomp=%u leak=%d\n",
        features, abi, mode, leak);
    return 0;
}
