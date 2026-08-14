// Credit-aggregation threshold unit test (E11 credit pacing hardening).
//
// The response credit grant path batches pending credit per request and
// submits it once the pending amount reaches the aggregation threshold
// (CAPSID_CREDIT_GRANT_THRESHOLD, default 16384). A threshold at or above
// the response window is a starvation hazard: for a long-lived stream the
// worker can only ever get back what it consumed, so pending credit would
// never reach a threshold larger than the window and the stream would
// stall until RESPONSE_END forces the flush.
//
// The clamp helper is a pure function so this boundary is unit-testable
// without sockets or sleeps: the effective threshold must never exceed
// window/4, zero must always mean "immediate grant" (off), and degenerate
// windows must degrade to immediate rather than stall.
#include "host/credit_limits.h"
#include "host/response_body_batch.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "test_host_credit_limits: " << message << std::endl;
        std::exit(1);
    }
}

struct QueuedBody {
    std::vector<std::uint8_t> bytes;
    bool credit_returned_early = false;
};

QueuedBody body(std::initializer_list<std::uint8_t> bytes,
                bool credit_returned_early) {
    return QueuedBody{std::vector<std::uint8_t>(bytes),
                      credit_returned_early};
}

void test_response_body_batching() {
    std::deque<QueuedBody> queue;
    queue.push_back(body({1, 2}, true));
    queue.push_back(body({3, 4}, true));
    queue.push_back(body({5, 6}, true));
    queue.push_back(body({7, 8}, true));
    std::size_t queued_bytes = 8;

    QueuedBody first = capsid::host::take_coalesced_response_body(
        &queue, &queued_bytes, 6);
    require(first.bytes == std::vector<std::uint8_t>({1, 2, 3, 4, 5, 6}),
            "same-credit frames did not coalesce in order");
    require(first.credit_returned_early,
            "coalescing changed the credit-return state");
    require(queue.size() == 1 && queued_bytes == 2,
            "coalescing broke queued-byte accounting");

    queue.clear();
    queue.push_back(body({1, 2}, true));
    queue.push_back(body({3, 4}, false));
    queue.push_back(body({5, 6}, true));
    queued_bytes = 6;
    first = capsid::host::take_coalesced_response_body(
        &queue, &queued_bytes, 64);
    require(first.bytes == std::vector<std::uint8_t>({1, 2}),
            "batch crossed an early-credit boundary");
    require(queue.size() == 2 && queued_bytes == 4,
            "credit-boundary stop broke queue accounting");

    QueuedBody second = capsid::host::take_coalesced_response_body(
        &queue, &queued_bytes, 64);
    require(second.bytes == std::vector<std::uint8_t>({3, 4}) &&
                !second.credit_returned_early,
            "completion-credit frame changed semantics");
    require(queue.size() == 1 && queued_bytes == 2,
            "completion-credit pop broke queue accounting");
}

}  // namespace

int main() {
    test_response_body_batching();
    // 0 stays 0: the off/backward-compat convention is untouched by the
    // clamp, so deployments that explicitly disable aggregation keep the
    // immediate-grant pacing.
    require(capsid::host::clamp_credit_grant_threshold(0, 65536) == 0,
            "explicit off did not stay off");
    require(capsid::host::clamp_credit_grant_threshold(0, 4) == 0,
            "explicit off did not stay off under a tiny window");

    // Values at or below window/4 pass through exactly.
    require(capsid::host::clamp_credit_grant_threshold(16384, 65536) ==
                16384,
            "default threshold did not pass through under the 64 KiB window");
    require(capsid::host::clamp_credit_grant_threshold(1024, 8192) == 1024,
            "small threshold did not pass through under an 8 KiB window");

    // Values above window/4 clamp down: pending credit is bounded by the
    // window, so a larger threshold could never be reached mid-stream.
    require(capsid::host::clamp_credit_grant_threshold(65536, 65536) ==
                16384,
            "threshold equal to the window did not clamp to window/4");
    require(capsid::host::clamp_credit_grant_threshold(32768, 8192) == 2048,
            "threshold above the window did not clamp to window/4");
    require(capsid::host::clamp_credit_grant_threshold(
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::uint32_t>::max()),
                16384) == 4096,
            "UINT32_MAX threshold did not clamp to window/4");

    // Degenerate windows degrade to immediate grant instead of stalling.
    require(capsid::host::clamp_credit_grant_threshold(16384, 0) == 0,
            "zero window did not degrade to immediate grant");
    require(capsid::host::clamp_credit_grant_threshold(16384, 4) == 1,
            "tiny window did not degrade to the minimum grant");

    std::cout << "PASS" << std::endl;
    return 0;
}
