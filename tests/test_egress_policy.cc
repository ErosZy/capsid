#include "egress_policy.h"
#include "capsid/runtime.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "test-egress-policy: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

capsid_egress_rule rule(capsid_egress_action action,
                       const char *target,
                       uint16_t port_start = 0,
                       uint16_t port_end = 0) {
    capsid_egress_rule value;
    capsid_egress_rule_init(&value);
    value.action = action;
    value.target = target;
    value.port_start = port_start;
    value.port_end = port_end;
    return value;
}

bool configure(capsid::EgressPolicy *compiled,
               capsid_egress_action default_action,
               const std::vector<capsid_egress_rule> &rules,
               std::string *error = NULL) {
    capsid_egress_policy policy;
    capsid_egress_policy_init(&policy);
    policy.default_action = default_action;
    policy.rules = rules.empty() ? NULL : &rules[0];
    policy.rule_count = static_cast<uint32_t>(rules.size());
    return compiled->configure(&policy, error);
}

sockaddr_storage address(const char *text, uint16_t port) {
    sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    sockaddr_in *v4 = reinterpret_cast<sockaddr_in *>(&storage);
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        return storage;
    }
    sockaddr_in6 *v6 = reinterpret_cast<sockaddr_in6 *>(&storage);
    if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        return storage;
    }
    fail(std::string("invalid test address: ") + text);
    return storage;
}

bool allows_address(const capsid::EgressPolicy &policy,
                    const char *text,
                    uint16_t port) {
    const sockaddr_storage storage = address(text, port);
    const socklen_t size =
        storage.ss_family == AF_INET ? sizeof(sockaddr_in)
                                    : sizeof(sockaddr_in6);
    return policy.allows_resolved_address(
        reinterpret_cast<const sockaddr *>(&storage), size, port);
}

void test_default_and_host_matching() {
    capsid::EgressPolicy policy;
    std::string error;
    require(configure(
                &policy,
                CAPSID_EGRESS_DENY,
                std::vector<capsid_egress_rule>(),
                &error),
            error);
    require(!policy.allows_host("example.com", 443),
            "empty deny policy granted a hostname");
    require(!policy.allows_host("93.184.216.34", 443),
            "empty deny policy granted a numeric address");

    std::vector<capsid_egress_rule> rules;
    rules.push_back(rule(
        CAPSID_EGRESS_ALLOW, "Api.Example.COM.", 443, 443));
    rules.push_back(rule(
        CAPSID_EGRESS_ALLOW, "*.service.example", 8000, 8999));
    require(configure(&policy, CAPSID_EGRESS_DENY, rules, &error), error);
    require(policy.allows_host("api.example.com", 443),
            "exact host normalization failed");
    require(!policy.allows_host("api.example.com", 80),
            "port scope was ignored");
    require(policy.allows_host("a.service.example.", 8443),
            "wildcard subdomain did not match");
    require(!policy.allows_host("service.example", 8443),
            "wildcard unexpectedly matched its apex");
    require(!policy.allows_host("evilservice.example", 8443),
            "wildcard ignored the DNS label boundary");
}

void test_deny_precedence() {
    capsid::EgressPolicy policy;
    std::string error;
    std::vector<capsid_egress_rule> rules;
    rules.push_back(rule(CAPSID_EGRESS_DENY, "blocked.example", 443, 443));
    rules.push_back(rule(CAPSID_EGRESS_ALLOW, "*.example", 443, 443));
    require(configure(&policy, CAPSID_EGRESS_DENY, rules, &error), error);
    require(policy.allows_host("ok.example", 443),
            "allow wildcard did not match");
    require(!policy.allows_host("blocked.example", 443),
            "deny did not override allow");

    rules.clear();
    rules.push_back(rule(CAPSID_EGRESS_ALLOW, "10.0.0.0/8"));
    rules.push_back(rule(CAPSID_EGRESS_DENY, "10.1.0.0/16"));
    require(configure(&policy, CAPSID_EGRESS_ALLOW, rules, &error), error);
    require(allows_address(policy, "10.2.3.4", 443),
            "explicit private CIDR allow was not honored");
    require(!allows_address(policy, "10.1.2.3", 443),
            "CIDR deny did not override a broader allow");
}

void test_protected_addresses_and_rebinding() {
    capsid::EgressPolicy policy;
    std::string error;
    require(configure(
                &policy,
                CAPSID_EGRESS_ALLOW,
                std::vector<capsid_egress_rule>(),
                &error),
            error);

    require(allows_address(policy, "93.184.216.34", 443),
            "public IPv4 was denied");
    require(allows_address(policy, "2606:2800:220:1:248:1893:25c8:1946", 443),
            "public IPv6 was denied");
    require(!allows_address(policy, "127.0.0.1", 80),
            "IPv4 loopback was granted");
    require(!allows_address(policy, "169.254.169.254", 80),
            "metadata/link-local address was granted");
    require(!allows_address(policy, "10.0.0.1", 80),
            "private IPv4 was granted");
    require(!allows_address(policy, "::1", 80),
            "IPv6 loopback was granted");
    require(!allows_address(policy, "fe80::1", 80),
            "IPv6 link-local was granted");
    require(!allows_address(policy, "fd00::1", 80),
            "IPv6 unique-local was granted");
    require(!allows_address(policy, "::ffff:127.0.0.1", 80),
            "IPv4-mapped loopback was granted");

    std::vector<capsid_egress_rule> rules;
    rules.push_back(rule(CAPSID_EGRESS_ALLOW, "rebind.example", 443, 443));
    require(configure(&policy, CAPSID_EGRESS_DENY, rules, &error), error);
    require(policy.allows_host("rebind.example", 443),
            "allowed DNS name failed preflight");
    require(!allows_address(policy, "169.254.169.254", 443),
            "allowed DNS name bypassed resolved-address protection");

    rules.push_back(rule(
        CAPSID_EGRESS_ALLOW, "169.254.169.254/32", 443, 443));
    require(configure(&policy, CAPSID_EGRESS_DENY, rules, &error), error);
    require(allows_address(policy, "169.254.169.254", 443),
            "explicit metadata address allow was not honored");
}

void test_numeric_targets_and_copy_lifetime() {
    capsid::EgressPolicy policy;
    std::string target("203.0.113.7/32");
    std::vector<capsid_egress_rule> rules;
    rules.push_back(rule(CAPSID_EGRESS_ALLOW, target.c_str(), 443, 443));
    std::string error;
    require(configure(&policy, CAPSID_EGRESS_DENY, rules, &error), error);
    target.assign("127.0.0.1/32");
    require(policy.allows_host("203.0.113.7", 443),
            "compiled policy retained the caller's target pointer");
    require(!policy.allows_host("127.0.0.1", 443),
            "caller mutation changed the compiled policy");
}

void require_invalid(const capsid_egress_rule &invalid,
                     const std::string &label) {
    capsid::EgressPolicy policy;
    std::vector<capsid_egress_rule> rules(1);
    std::memcpy(&rules[0], &invalid, sizeof(invalid));
    std::string error;
    require(!configure(&policy, CAPSID_EGRESS_DENY, rules, &error),
            label + " was accepted");
    require(!error.empty(), label + " produced no validation error");
}

void test_malformed_rules() {
    require_invalid(
        rule(CAPSID_EGRESS_ALLOW, "*example.com"),
        "malformed wildcard");
    require_invalid(
        rule(CAPSID_EGRESS_ALLOW, "example..com"),
        "empty hostname label");
    require_invalid(
        rule(CAPSID_EGRESS_ALLOW, "10.1.2.3/8"),
        "non-canonical CIDR");
    require_invalid(
        rule(CAPSID_EGRESS_ALLOW, "10.0.0.0/33"),
        "oversized IPv4 prefix");
    require_invalid(
        rule(CAPSID_EGRESS_ALLOW, "example.com", 443, 80),
        "reversed port range");

    capsid_egress_rule invalid = rule(
        CAPSID_EGRESS_ALLOW, "example.com");
    invalid.reserved = 1;
    require_invalid(invalid, "nonzero rule reserved field");

    uint32_t invalid_enum = 999;
    invalid = rule(CAPSID_EGRESS_ALLOW, "example.com");
    std::memcpy(
        &invalid.action,
        &invalid_enum,
        sizeof(invalid_enum));
    require_invalid(invalid, "unknown rule action");

    capsid::EgressPolicy policy;
    std::vector<capsid_egress_rule> valid_rules;
    valid_rules.push_back(rule(
        CAPSID_EGRESS_ALLOW, "kept.example", 443, 443));
    std::string error;
    require(
        configure(
            &policy,
            CAPSID_EGRESS_DENY,
            valid_rules,
            &error),
        error);
    capsid_egress_policy invalid_policy;
    capsid_egress_policy_init(&invalid_policy);
    std::memcpy(
        &invalid_policy.default_action,
        &invalid_enum,
        sizeof(invalid_enum));
    require(
        !policy.configure(&invalid_policy, &error),
        "unknown default action was accepted");
    require(
        policy.allows_host("kept.example", 443),
        "failed configure partially mutated egress policy");
}

}  // namespace

int main() {
    test_default_and_host_matching();
    test_deny_precedence();
    test_protected_addresses_and_rebinding();
    test_numeric_targets_and_copy_lifetime();
    test_malformed_rules();
    return 0;
}
