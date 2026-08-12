// Poll-timeout saturation unit test (release-standard hardening).
//
// poll(2) takes a signed int timeout: negative values mean "wait forever".
// The Host's public timeouts are uint32_t (Admin options) or derive from
// steady_clock deadlines (worker event source), both of which can exceed
// INT_MAX milliseconds. Before the hardening batch, both call sites cast
// the large value straight into int, wrapping UINT32_MAX into -1 and
// turning a large-but-valid timeout into an unbounded wait.
//
// The saturation helper is a pure function so this boundary is unit-testable
// without sockets or sleeps: every value at and above INT_MAX must clamp,
// and every value below must pass through exactly.
#include "host/poll_limits.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_host_poll_limits: " << message << std::endl;
        std::exit(1);
    }
}

}  // namespace

int main() {
    // Exact pass-through below INT_MAX (poll accepts these verbatim).
    require(capsid::host::poll_timeout_ms(0) == 0,
            "zero timeout did not pass through");
    require(capsid::host::poll_timeout_ms(1) == 1,
            "one-millisecond timeout did not pass through");
    require(capsid::host::poll_timeout_ms(999) == 999,
            "sub-second timeout did not pass through");
    require(capsid::host::poll_timeout_ms(
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()) -
                1) == std::numeric_limits<int>::max() - 1,
            "INT_MAX-1 timeout did not pass through");

    // Saturation at INT_MAX (the largest value poll accepts as "finite").
    require(capsid::host::poll_timeout_ms(
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())) ==
                std::numeric_limits<int>::max(),
            "INT_MAX timeout did not saturate to itself");

    // Values that would wrap negative without the clamp. Each of these
    // must return INT_MAX, never a negative poll timeout (which would
    // block forever on a wedged peer).
    require(capsid::host::poll_timeout_ms(
                static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()) +
                1) == std::numeric_limits<int>::max(),
            "INT_MAX+1 wrapped past the poll timeout ceiling");
    require(capsid::host::poll_timeout_ms(
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max())) ==
                std::numeric_limits<int>::max(),
            "UINT32_MAX wrapped into an unbounded poll timeout");
    require(capsid::host::poll_timeout_ms(
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint64_t>::max())) ==
                std::numeric_limits<int>::max(),
            "UINT64_MAX wrapped into an unbounded poll timeout");

    std::cout << "PASS" << std::endl;
    return 0;
}
