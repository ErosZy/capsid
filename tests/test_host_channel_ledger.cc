// Host channel capacity ledger + protocol selection Red tests
// (GLM plan T08): separate counters, upgrade commit semantics,
// failed-reservation restoration, retire gating, service aggregates,
// channel config validation, and generation identity sensitivity to
// protocol limits.
#include "host/channel_ledger.h"
#include "host/generation_identity.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using capsid::host::ChannelLedger;
using capsid::host::ServiceChannelBudget;

void test_separate_counters_and_upgrade_commit() {
    // Red item: 零 HTTP inflight 仍有活跃 WS — the counters are
    // separate and observable after an upgrade commits.
    ChannelLedger::Limits limits;
    limits.max_http_inflight = 4;
    limits.max_channels = 4;
    ChannelLedger ledger(limits);

    require(ledger.try_reserve_http(), "http reserved");
    require(ledger.http_inflight() == 1, "http counted");
    require(ledger.commit_upgrade_from_http(), "upgrade commits");
    // the HTTP slot returned; the channel slot is held
    require(ledger.http_inflight() == 0,
            "upgrade releases the HTTP slot");
    require(ledger.active_channels() == 1,
            "upgrade keeps the channel slot");
    // zero HTTP inflight WITH an active channel — health checks see
    // WebSocket liveness through the channel counter, not a phantom
    // HTTP response
    require(ledger.http_inflight() == 0 && ledger.active_channels() > 0,
            "zero HTTP inflight with an active channel");

    // channel budget exhausts independently of HTTP
    for (int i = 0; i < 3; ++i) {
        require(ledger.try_commit_channel(), "channel budget");
    }
    require(!ledger.try_commit_channel(), "channel ceiling refuses");
    require(ledger.try_reserve_http(), "HTTP still reservable");
    ledger.release_channel();
    require(ledger.try_commit_channel(),
            "release restores channel capacity");
}

void test_failed_reservation_restores_capacity() {
    // Red item: 失败预留恢复容量
    ChannelLedger::Limits limits;
    limits.max_channels = 1;
    limits.max_pending_dials = 1;
    ChannelLedger ledger(limits);

    require(ledger.try_commit_channel(), "first channel in");
    require(!ledger.try_commit_channel(), "second refused");
    require(ledger.active_channels() == 1,
            "refusal does not mutate the counter");
    // the single dial slot goes to one reservation; the second refuses
    require(ledger.try_reserve_dial(), "first dial in");
    require(!ledger.try_reserve_dial(), "dial ceiling refuses");
    require(ledger.pending_dials() == 1,
            "refused dial leaves no partial state");
    ledger.release_dial();
    require(ledger.pending_dials() == 0,
            "dial release restores capacity");

    // upgrade commit failure keeps BOTH sides untouched: with both
    // HTTP slots drained there is nothing to commit from
    ChannelLedger::Limits tight;
    tight.max_http_inflight = 2;
    tight.max_channels = 0;
    ChannelLedger tight_ledger(tight);
    require(tight_ledger.try_reserve_http(), "http in");
    require(tight_ledger.try_reserve_http(), "http in again");
    require(tight_ledger.commit_upgrade_from_http(),
            "commit succeeds under budget");
    require(tight_ledger.commit_upgrade_from_http(),
            "second commit drains the last slot");
    require(!tight_ledger.commit_upgrade_from_http(),
            "commit without an HTTP slot fails");
    require(tight_ledger.active_channels() == 2,
            "failed commit changes nothing");
    require(tight_ledger.http_inflight() == 0,
            "both HTTP slots released");
}

void test_retire_gating() {
    // Red item: retire 后不再接新 channel；既有通道继续可见
    ChannelLedger::Limits limits;
    limits.max_channels = 4;
    ChannelLedger ledger(limits);
    require(ledger.try_commit_channel(), "channel before retire");
    ledger.retire();
    require(ledger.retired(), "retired flag");
    require(!ledger.try_commit_channel(), "no new channels after retire");
    require(!ledger.try_reserve_dial(), "no new dials after retire");
    require(ledger.active_channels() == 1,
            "existing channel stays observable");
    ledger.release_channel();
    require(ledger.active_channels() == 0, "drain after retire");
    // HTTP continues draining: retire only stops NEW channel traffic
    require(ledger.try_reserve_http(), "http drain unaffected");
    ledger.release_http();
}

void test_service_aggregate() {
    // listener 与整服务都有 aggregate budget
    ServiceChannelBudget service;
    service.set_channel_ceiling(2);
    service.set_dial_ceiling(1);

    ChannelLedger::Limits a_limits;
    ChannelLedger a(a_limits, &service);
    ChannelLedger::Limits b_limits;
    ChannelLedger b(b_limits, &service);

    require(a.try_commit_channel() && b.try_commit_channel(),
            "service budget covers two listeners");
    ChannelLedger::Limits c_limits;
    ChannelLedger c(c_limits, &service);
    require(!c.try_commit_channel(),
            "service ceiling refuses the third listener");
    require(service.channels() == 2, "service count");
    a.release_channel();
    require(c.try_commit_channel(), "release restores the service pool");

    require(a.try_reserve_dial(), "dial one");
    require(!b.try_reserve_dial(),
            "service dial ceiling refuses the second");
    a.release_dial();
    require(b.try_reserve_dial(), "dial release restores");
}

void test_generation_identity_sensitivity() {
    // Red item: 相同 bundle 不同 limits 的 generation — protocol
    // limits take part in the identity
    capsid::protocol::ChannelLimits defaults;
    capsid::protocol::ChannelLimits bigger;
    bigger.max_channels = 256;
    bigger.max_message_bytes = 4u * 1024u * 1024u;

    const std::string digest_default =
        capsid::host::compute_protocol_limits_digest(4, defaults);
    const std::string digest_bigger =
        capsid::host::compute_protocol_limits_digest(4, bigger);
    const std::string digest_v3 =
        capsid::host::compute_protocol_limits_digest(3, defaults);
    // an all-zero config and the explicit deployment defaults agree
    capsid::protocol::ChannelLimits explicit_defaults;
    explicit_defaults.max_channels = 128;
    explicit_defaults.max_message_bytes = 1024u * 1024u;
    explicit_defaults.reassembly_budget_bytes = 16u * 1024u * 1024u;
    explicit_defaults.send_budget_bytes = 16u * 1024u * 1024u;
    explicit_defaults.close_timeout_ms = 5000;
    explicit_defaults.dial_timeout_ms = 10000;
    explicit_defaults.per_channel_queue_bytes = 4u * 1024u * 1024u;

    require(digest_default != digest_bigger,
            "different limits are different identities");
    require(digest_default != digest_v3,
            "wire version is part of the identity");
    require(digest_default ==
                capsid::host::compute_protocol_limits_digest(
                    4, explicit_defaults),
            "defaulted and explicit-default configs agree");

    capsid::host::GenerationIdentityInput base;
    base.application_id = "app";
    base.source_digest = "sha256:aa";
    base.protocol_limits_digest = digest_default;
    const std::string generation_a =
        capsid::host::compute_generation_digest(base);
    base.protocol_limits_digest = digest_bigger;
    const std::string generation_b =
        capsid::host::compute_generation_digest(base);
    require(generation_a != generation_b,
            "same bundle under different limits is a different "
            "generation");
}

}  // namespace

int main() {
    {
        auto service=std::make_shared<capsid::host::ServiceChannelBudget>();
        service->set_queue_ceiling(512);
        auto owner=service->reserve_queue(270);
        require(owner && service->queued_bytes()==270,"empty network message metadata is charged");
        require(!service->reserve_queue(270) && service->queued_bytes()==270,"aggregate network queue refuses before allocation");
        auto in_flight=owner;owner.reset();
        require(service->queued_bytes()==270,"in-flight writer retains quota after session queue releases it");
        in_flight.reset();require(service->queued_bytes()==0,"last buffer owner returns service bytes");
        service->set_queue_ceiling(0);
        owner=service->reserve_queue(UINT64_MAX);
        require(owner && !service->reserve_queue(1),"queue accounting cannot wrap at UINT64_MAX");
        owner.reset();require(service->queued_bytes()==0,"overflow refusal leaves accounting releasable");
    }
    {
        capsid::host::ServiceChannelBudget service;
        service.release_channel();
        service.release_dial();
        require(service.channels() == 0 && service.dials() == 0,
                "empty service cleanup cannot underflow its counters");
        require(service.try_reserve_channel() && service.try_reserve_dial(),
                "reservations still work after repeated cleanup");
        service.release_channel();
        service.release_channel();
        service.release_dial();
        service.release_dial();
        require(service.channels() == 0 && service.dials() == 0,
                "repeated service cleanup remains at zero");
    }
    test_separate_counters_and_upgrade_commit();
    test_failed_reservation_restores_capacity();
    test_retire_gating();
    test_service_aggregate();
    test_generation_identity_sensitivity();
    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("host channel ledger + protocol identity: PASS\n");
    return 0;
}
