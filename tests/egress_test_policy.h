#ifndef CAPSID_TEST_EGRESS_POLICY_H
#define CAPSID_TEST_EGRESS_POLICY_H

#include "capsid/runtime.h"

/*
 * Deterministic network fixtures listen on loopback. Production policies do
 * not grant loopback implicitly, so every test that expects direct Fetch
 * success opts in explicitly through the same public ABI as an embedder.
 */
class LoopbackEgressPolicy {
public:
    LoopbackEgressPolicy() {
        capsid_egress_rule_init(&rules_[0]);
        rules_[0].action = CAPSID_EGRESS_ALLOW;
        rules_[0].target = "127.0.0.0/8";

        capsid_egress_rule_init(&rules_[1]);
        rules_[1].action = CAPSID_EGRESS_ALLOW;
        rules_[1].target = "::1/128";

        capsid_egress_policy_init(&policy_);
        policy_.default_action = CAPSID_EGRESS_ALLOW;
        policy_.rules = rules_;
        policy_.rule_count = 2;
    }

    void attach(capsid_worker_config *config) const {
        config->egress_policy = &policy_;
    }

private:
    capsid_egress_rule rules_[2];
    capsid_egress_policy policy_;
};

#endif
