// Credit-aggregation threshold clamp (E11 credit pacing hardening).
//
// The response credit grant path batches pending credit per request and
// submits it once the pending amount reaches the aggregation threshold.
// Pending credit can never exceed the response window, so a threshold at
// or above the window would never be reached mid-stream: the worker would
// stall until RESPONSE_END forces the flush. The effective threshold is
// therefore clamped to window/4 — large enough to keep the batching win,
// small enough that a long-lived stream always regains credit before the
// window drains fully. Zero always means immediate grant (off), and a
// degenerate window degrades to immediate grant rather than stall.
//
// Kept as a pure inline function so the boundary is unit-tested without
// sockets or sleeps (tests/test_host_credit_limits.cc).
#ifndef CAPSID_SRC_HOST_CREDIT_LIMITS_H_
#define CAPSID_SRC_HOST_CREDIT_LIMITS_H_

#include <algorithm>
#include <cstdint>

namespace capsid::host {

inline std::uint32_t clamp_credit_grant_threshold(std::uint64_t requested,
                                                  std::uint32_t window) {
    if (requested == 0) {
        return 0;  // explicit off: keep immediate-grant pacing
    }
    const std::uint32_t cap = window / 4;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(requested, cap));
}

}  // namespace capsid::host

#endif  // CAPSID_SRC_HOST_CREDIT_LIMITS_H_
