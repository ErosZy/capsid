// Poll-timeout saturation (release-standard hardening).
//
// poll(2) takes a signed int timeout: negative values mean "wait forever".
// Host call sites hold timeouts wider than int — the Admin options are
// uint32_t and the worker event source derives millisecond deadlines from
// steady_clock — so a large-but-valid value cast straight into int wraps
// negative and silently turns a timeout into an unbounded wait.
//
// This helper is the single clamp for every poll call site: values at or
// above INT_MAX saturate to INT_MAX (the largest finite poll timeout), and
// smaller values pass through exactly. Kept as a pure inline function so
// the boundary is unit-tested without sockets or sleeps
// (tests/test_host_poll_limits.cc).
#ifndef CAPSID_SRC_HOST_POLL_LIMITS_H_
#define CAPSID_SRC_HOST_POLL_LIMITS_H_

#include <cstdint>
#include <limits>

namespace capsid::host {

inline int poll_timeout_ms(std::uint64_t timeout_ms) {
    constexpr std::uint64_t kIntMax =
        static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return timeout_ms >= kIntMax ? std::numeric_limits<int>::max()
                                 : static_cast<int>(timeout_ms);
}

}  // namespace capsid::host

#endif  // CAPSID_SRC_HOST_POLL_LIMITS_H_
