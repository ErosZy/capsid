#ifndef CAPSID_EGRESS_POLICY_H
#define CAPSID_EGRESS_POLICY_H

#include "capsid/runtime.h"

#include <stdint.h>
#include <sys/socket.h>

#include <string>
#include <vector>

namespace capsid {

// Why a decision denied. Carried on EgressDecision so the worker can
// report a diagnostic message instead of a bare "denied" — the two-stage
// (host then resolved-address) check made denials hard to diagnose.
enum class EgressDenyReason {
    kNone = 0,      // allowed, or no decision was made
    kNoMatch,       // no rule matched (default-deny applies)
    kExplicitDeny,  // a DENY rule matched
    kProtected,     // protected range without an explicit rule
};

struct EgressDecision {
    bool allowed;
    uint32_t rule_id;
    EgressDenyReason deny_reason;

    EgressDecision(bool value = false,
                   uint32_t matched_rule = 0,
                   EgressDenyReason reason = EgressDenyReason::kNone)
        : allowed(value), rule_id(matched_rule), deny_reason(reason) {}
};

class EgressPolicy {
public:
    EgressPolicy();

    bool configure(const capsid_egress_policy *policy, std::string *error);
    EgressDecision decide_host(const std::string &host,
                               uint16_t port) const;
    // DNS has no destination port yet. It is authorized only when at least
    // one allow rule covers the normalized host/address at some port.
    EgressDecision decide_host_any_port(const std::string &host) const;
    EgressDecision decide_resolved_address(
        const struct sockaddr *address,
        socklen_t address_size,
        uint16_t expected_port) const;
    // Connect-stage decision for a hostname that already passed
    // decide_host: only explicit address rules apply, and an unmatched
    // address is allowed. The address being connected came from the
    // caller's own resolution of the authorized hostname, so the
    // protected-range fallback (an SSRF guard for numeric hosts, which
    // decide_host still applies) would wrongly deny authorized domains
    // that legitimately resolve into private ranges.
    EgressDecision decide_resolved_address_authoritative(
        const struct sockaddr *address,
        socklen_t address_size,
        uint16_t expected_port) const;
    bool allows_host(const std::string &host, uint16_t port) const;
    bool allows_resolved_address(const struct sockaddr *address,
                                 socklen_t address_size,
                                 uint16_t expected_port) const;
    capsid_permission_state query_state() const;

    capsid_egress_action default_action() const { return default_action_; }
    size_t rule_count() const { return rules_.size(); }

private:
    struct Rule {
        capsid_egress_action action;
        bool address;
        bool wildcard;
        int family;
        uint8_t network[16];
        uint8_t prefix;
        uint16_t port_start;
        uint16_t port_end;
        uint32_t rule_id;
        std::string host;

        Rule();
    };

    EgressDecision decide_address(const uint8_t *bytes,
                                  int family,
                                  uint16_t port,
                                  bool apply_default,
                                  bool protect_unmatched) const;
    EgressDecision decide_resolved_address_impl(
        const struct sockaddr *address,
        socklen_t address_size,
        uint16_t expected_port,
        bool protect_unmatched) const;

    capsid_egress_action default_action_;
    std::vector<Rule> rules_;
};

}  // namespace capsid

#endif
