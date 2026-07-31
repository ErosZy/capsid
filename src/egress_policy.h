#ifndef CAPSID_EGRESS_POLICY_H
#define CAPSID_EGRESS_POLICY_H

#include "capsid/runtime.h"

#include <stdint.h>
#include <sys/socket.h>

#include <string>
#include <vector>

namespace capsid {

struct EgressDecision {
    bool allowed;
    uint32_t rule_id;

    EgressDecision(bool value = false, uint32_t matched_rule = 0)
        : allowed(value), rule_id(matched_rule) {}
};

class EgressPolicy {
public:
    EgressPolicy();

    bool configure(const capsid_egress_policy *policy, std::string *error);
    EgressDecision decide_host(const std::string &host,
                               uint16_t port) const;
    EgressDecision decide_resolved_address(
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
                                  bool apply_default) const;

    capsid_egress_action default_action_;
    std::vector<Rule> rules_;
};

}  // namespace capsid

#endif
