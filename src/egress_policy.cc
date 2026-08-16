#include "egress_policy.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

extern "C" {

void capsid_egress_rule_init(capsid_egress_rule *rule) {
    if (!rule) {
        return;
    }
    std::memset(rule, 0, sizeof(*rule));
    rule->struct_size = sizeof(*rule);
    rule->action = CAPSID_EGRESS_DENY;
}

void capsid_egress_policy_init(capsid_egress_policy *policy) {
    if (!policy) {
        return;
    }
    std::memset(policy, 0, sizeof(*policy));
    policy->struct_size = sizeof(*policy);
    policy->default_action = CAPSID_EGRESS_DENY;
}

}

namespace {

void set_error(std::string *error, const std::string &message) {
    if (error) {
        *error = message;
    }
}

bool valid_port_range(uint16_t start, uint16_t end) {
    return (start == 0 && end == 0) ||
           (start != 0 && end >= start);
}

uint32_t egress_action_value(const capsid_egress_action &action) {
    static_assert(
        sizeof(capsid_egress_action) == sizeof(uint32_t),
        "public egress action ABI must be 32-bit");
    uint32_t value = 0;
    std::memcpy(&value, &action, sizeof(value));
    return value;
}

bool port_matches(uint16_t start, uint16_t end, uint16_t port) {
    return start == 0 || (port >= start && port <= end);
}

bool normalize_hostname(const std::string &input,
                        bool allow_wildcard,
                        std::string *host,
                        bool *wildcard) {
    if (!host || !wildcard || input.empty()) {
        return false;
    }
    std::string value = input;
    if (value.size() > 1 && value[value.size() - 1] == '.') {
        value.resize(value.size() - 1);
    }
    *wildcard = false;
    if (value.compare(0, 2, "*.") == 0) {
        if (!allow_wildcard) {
            return false;
        }
        *wildcard = true;
        value.erase(0, 2);
    }
    if (value.empty() || value.size() > 253 ||
        value.find('*') != std::string::npos) {
        return false;
    }

    size_t label_start = 0;
    for (size_t index = 0; index <= value.size(); ++index) {
        if (index != value.size() && value[index] != '.') {
            const unsigned char ch =
                static_cast<unsigned char>(value[index]);
            if (ch >= 'A' && ch <= 'Z') {
                value[index] = static_cast<char>(ch - 'A' + 'a');
            } else if (!((ch >= 'a' && ch <= 'z') ||
                         (ch >= '0' && ch <= '9') ||
                         ch == '-')) {
                return false;
            }
            continue;
        }
        const size_t label_size = index - label_start;
        if (label_size == 0 || label_size > 63 ||
            value[label_start] == '-' || value[index - 1] == '-') {
            return false;
        }
        label_start = index + 1;
    }
    *host = value;
    return true;
}

bool prefix_matches(const uint8_t *address,
                    const uint8_t *network,
                    uint8_t prefix) {
    const size_t whole_bytes = prefix / 8;
    const uint8_t remaining = prefix % 8;
    if (whole_bytes != 0 &&
        std::memcmp(address, network, whole_bytes) != 0) {
        return false;
    }
    if (remaining == 0) {
        return true;
    }
    const uint8_t mask =
        static_cast<uint8_t>(0xffu << (8u - remaining));
    return (address[whole_bytes] & mask) ==
           (network[whole_bytes] & mask);
}

bool network_is_canonical(const uint8_t *address,
                          size_t size,
                          uint8_t prefix) {
    const size_t whole_bytes = prefix / 8;
    const uint8_t remaining = prefix % 8;
    if (remaining != 0) {
        const uint8_t host_mask =
            static_cast<uint8_t>((1u << (8u - remaining)) - 1u);
        if ((address[whole_bytes] & host_mask) != 0) {
            return false;
        }
    }
    const size_t tail_start = whole_bytes + (remaining == 0 ? 0 : 1);
    for (size_t index = tail_start; index < size; ++index) {
        if (address[index] != 0) {
            return false;
        }
    }
    return true;
}

bool is_ipv4_mapped(const uint8_t *address) {
    static const uint8_t prefix[12] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
    };
    return std::memcmp(address, prefix, sizeof(prefix)) == 0;
}

bool is_protected_ipv4(const uint8_t *bytes) {
    const uint32_t value =
        (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) |
        static_cast<uint32_t>(bytes[3]);
    return
        (value & UINT32_C(0xff000000)) == UINT32_C(0x00000000) ||
        (value & UINT32_C(0xff000000)) == UINT32_C(0x0a000000) ||
        (value & UINT32_C(0xffc00000)) == UINT32_C(0x64400000) ||
        (value & UINT32_C(0xff000000)) == UINT32_C(0x7f000000) ||
        (value & UINT32_C(0xffff0000)) == UINT32_C(0xa9fe0000) ||
        (value & UINT32_C(0xfff00000)) == UINT32_C(0xac100000) ||
        (value & UINT32_C(0xffffff00)) == UINT32_C(0xc0000000) ||
        (value & UINT32_C(0xffffff00)) == UINT32_C(0xc0000200) ||
        (value & UINT32_C(0xffff0000)) == UINT32_C(0xc0a80000) ||
        (value & UINT32_C(0xfffe0000)) == UINT32_C(0xc6120000) ||
        (value & UINT32_C(0xffffff00)) == UINT32_C(0xc6336400) ||
        (value & UINT32_C(0xffffff00)) == UINT32_C(0xcb007100) ||
        (value & UINT32_C(0xf0000000)) == UINT32_C(0xe0000000) ||
        (value & UINT32_C(0xf0000000)) == UINT32_C(0xf0000000);
}

bool is_protected_ipv6(const uint8_t *bytes) {
    if (is_ipv4_mapped(bytes)) {
        return is_protected_ipv4(bytes + 12);
    }
    static const uint8_t zero96[12] = {};
    if (std::memcmp(bytes, zero96, sizeof(zero96)) == 0) {
        return true;
    }
    if ((bytes[0] & 0xfeu) == 0xfcu ||
        (bytes[0] == 0xfeu && (bytes[1] & 0xc0u) == 0x80u) ||
        bytes[0] == 0xffu) {
        return true;
    }
    static const uint8_t discard_prefix[8] =
        { 0x01, 0x00, 0, 0, 0, 0, 0, 0 };
    if (std::memcmp(bytes, discard_prefix, sizeof(discard_prefix)) == 0) {
        return true;
    }
    static const uint8_t documentation_prefix[4] =
        { 0x20, 0x01, 0x0d, 0xb8 };
    return std::memcmp(
               bytes,
               documentation_prefix,
               sizeof(documentation_prefix)) == 0;
}

bool host_rule_matches(const std::string &host,
                       const std::string &rule,
                       bool wildcard) {
    if (!wildcard) {
        return host == rule;
    }
    return host.size() > rule.size() + 1 &&
           host[host.size() - rule.size() - 1] == '.' &&
           host.compare(host.size() - rule.size(), rule.size(), rule) == 0;
}

bool parse_numeric(const std::string &input,
                   int *family,
                   uint8_t *bytes) {
    std::string value = input;
    if (value.size() >= 2 && value[0] == '[' &&
        value[value.size() - 1] == ']') {
        value = value.substr(1, value.size() - 2);
    }
    if (inet_pton(AF_INET, value.c_str(), bytes) == 1) {
        *family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, value.c_str(), bytes) == 1) {
        *family = AF_INET6;
        return true;
    }
    return false;
}

}  // namespace

namespace capsid {

EgressPolicy::Rule::Rule()
    : action(CAPSID_EGRESS_DENY),
      address(false),
      wildcard(false),
      family(AF_UNSPEC),
      prefix(0),
      port_start(0),
      port_end(0),
      rule_id(0) {
    std::memset(network, 0, sizeof(network));
}

EgressPolicy::EgressPolicy()
    : default_action_(CAPSID_EGRESS_DENY) {}

bool EgressPolicy::configure(const capsid_egress_policy *policy,
                             std::string *error) {
    if (error) {
        error->clear();
    }
    if (!policy) {
        rules_.clear();
        default_action_ = CAPSID_EGRESS_DENY;
        return true;
    }
    if (policy->struct_size != sizeof(capsid_egress_policy)) {
        set_error(error, "invalid egress policy struct_size");
        return false;
    }
    const uint32_t default_action_value =
        egress_action_value(policy->default_action);
    if (default_action_value > CAPSID_EGRESS_ALLOW ||
        policy->reserved != 0 ||
        policy->rule_count > 256 ||
        (policy->rule_count != 0 && !policy->rules)) {
        set_error(error, "invalid egress policy descriptor");
        return false;
    }

    std::vector<Rule> compiled;
    compiled.reserve(policy->rule_count);
    std::set<uint32_t> explicit_rule_ids;
    for (uint32_t index = 0; index < policy->rule_count; ++index) {
        const capsid_egress_rule &source = policy->rules[index];
        const uint32_t action_value =
            egress_action_value(source.action);
        if (source.struct_size != sizeof(capsid_egress_rule) ||
            action_value > CAPSID_EGRESS_ALLOW ||
            !source.target || source.reserved != 0 ||
            !valid_port_range(source.port_start, source.port_end)) {
            set_error(error, "invalid egress rule descriptor");
            return false;
        }
        const size_t target_size = std::strlen(source.target);
        if (target_size == 0 || target_size > 253) {
            set_error(error, "invalid egress rule target length");
            return false;
        }

        Rule rule;
        rule.action =
            static_cast<capsid_egress_action>(action_value);
        rule.port_start = source.port_start;
        rule.port_end = source.port_end;
        if (source.rule_id != 0 &&
            !explicit_rule_ids.insert(source.rule_id).second) {
            set_error(error, "duplicate egress rule_id");
            return false;
        }
        rule.rule_id = source.rule_id != 0
                           ? source.rule_id
                           : index + 1;
        const std::string target(source.target, target_size);
        const size_t slash = target.find('/');
        uint8_t address_bytes[16] = {};
        int family = AF_UNSPEC;
        if (slash != std::string::npos) {
            if (target.find('/', slash + 1) != std::string::npos ||
                slash == 0 || slash + 1 == target.size()) {
                set_error(error, "invalid egress CIDR");
                return false;
            }
            const std::string address_text = target.substr(0, slash);
            const std::string prefix_text = target.substr(slash + 1);
            char *end = NULL;
            errno = 0;
            const unsigned long prefix =
                std::strtoul(prefix_text.c_str(), &end, 10);
            if (errno != 0 || !end || *end != '\0' ||
                !parse_numeric(address_text, &family, address_bytes) ||
                (family == AF_INET && prefix > 32) ||
                (family == AF_INET6 && prefix > 128) ||
                (family == AF_INET6 &&
                 is_ipv4_mapped(address_bytes)) ||
                !network_is_canonical(
                    address_bytes,
                    family == AF_INET ? 4 : 16,
                    static_cast<uint8_t>(prefix))) {
                set_error(error, "invalid or non-canonical egress CIDR");
                return false;
            }
            rule.address = true;
            rule.family = family;
            rule.prefix = static_cast<uint8_t>(prefix);
            std::memcpy(
                rule.network,
                address_bytes,
                family == AF_INET ? 4 : 16);
        } else if (parse_numeric(target, &family, address_bytes)) {
            if (family == AF_INET6 && is_ipv4_mapped(address_bytes)) {
                rule.family = AF_INET;
                rule.prefix = 32;
                std::memcpy(rule.network, address_bytes + 12, 4);
            } else {
                rule.family = family;
                rule.prefix = family == AF_INET ? 32 : 128;
                std::memcpy(
                    rule.network,
                    address_bytes,
                    family == AF_INET ? 4 : 16);
            }
            rule.address = true;
        } else {
            if (!normalize_hostname(
                    target, true, &rule.host, &rule.wildcard)) {
                set_error(error, "invalid egress hostname pattern");
                return false;
            }
        }
        compiled.push_back(rule);
    }

    default_action_ =
        static_cast<capsid_egress_action>(default_action_value);
    rules_.swap(compiled);
    return true;
}

EgressDecision EgressPolicy::decide_host(
    const std::string &input,
    uint16_t port) const {
    if (port == 0) {
        return EgressDecision();
    }
    uint8_t bytes[16] = {};
    int family = AF_UNSPEC;
    if (parse_numeric(input, &family, bytes)) {
        if (family == AF_INET6 && is_ipv4_mapped(bytes)) {
            return decide_address(bytes + 12, AF_INET, port, true, true);
        }
        return decide_address(bytes, family, port, true, true);
    }

    std::string host;
    bool wildcard = false;
    if (!normalize_hostname(input, false, &host, &wildcard)) {
        return EgressDecision();
    }
    bool allowed = false;
    uint32_t allow_rule_id = 0;
    for (std::vector<Rule>::const_iterator it = rules_.begin();
         it != rules_.end();
         ++it) {
        if (it->address ||
            !port_matches(it->port_start, it->port_end, port) ||
            !host_rule_matches(host, it->host, it->wildcard)) {
            continue;
        }
        if (it->action == CAPSID_EGRESS_DENY) {
            return EgressDecision(false, it->rule_id,
                                  EgressDenyReason::kExplicitDeny);
        }
        allowed = true;
        if (allow_rule_id == 0) {
            allow_rule_id = it->rule_id;
        }
    }
    if (allowed) {
        return EgressDecision(true, allow_rule_id);
    }
    return EgressDecision(
        default_action_ == CAPSID_EGRESS_ALLOW, 0,
        default_action_ == CAPSID_EGRESS_ALLOW
            ? EgressDenyReason::kNone
            : EgressDenyReason::kNoMatch);
}

EgressDecision EgressPolicy::decide_host_any_port(
    const std::string &input) const {
    uint8_t bytes[16] = {};
    int family = AF_UNSPEC;
    const bool numeric = parse_numeric(input, &family, bytes);
    const uint8_t *address_bytes = bytes;
    if (numeric && family == AF_INET6 && is_ipv4_mapped(bytes)) {
        family = AF_INET;
        address_bytes = bytes + 12;
    }

    std::string host;
    bool input_wildcard = false;
    if (!numeric &&
        !normalize_hostname(input, false, &host, &input_wildcard)) {
        return EgressDecision();
    }
    (void) input_wildcard;

    for (std::vector<Rule>::const_iterator it = rules_.begin();
         it != rules_.end(); ++it) {
        if (it->action != CAPSID_EGRESS_ALLOW) {
            continue;
        }
        const bool matches = numeric
            ? it->address && it->family == family &&
                  prefix_matches(address_bytes, it->network, it->prefix)
            : !it->address &&
                  host_rule_matches(host, it->host, it->wildcard);
        if (matches) {
            return EgressDecision(true, it->rule_id);
        }
    }
    return EgressDecision(
        default_action_ == CAPSID_EGRESS_ALLOW,
        0,
        default_action_ == CAPSID_EGRESS_ALLOW
            ? EgressDenyReason::kNone
            : EgressDenyReason::kNoMatch);
}

EgressDecision EgressPolicy::decide_address(
    const uint8_t *bytes,
    int family,
    uint16_t port,
    bool apply_default,
    bool protect_unmatched) const {
    bool allowed = false;
    uint32_t allow_rule_id = 0;
    for (std::vector<Rule>::const_iterator it = rules_.begin();
         it != rules_.end();
         ++it) {
        if (!it->address || it->family != family ||
            !port_matches(it->port_start, it->port_end, port) ||
            !prefix_matches(bytes, it->network, it->prefix)) {
            continue;
        }
        if (it->action == CAPSID_EGRESS_DENY) {
            return EgressDecision(false, it->rule_id,
                                  EgressDenyReason::kExplicitDeny);
        }
        allowed = true;
        if (allow_rule_id == 0) {
            allow_rule_id = it->rule_id;
        }
    }
    if (allowed) {
        return EgressDecision(true, allow_rule_id);
    }
    if (protect_unmatched &&
        ((family == AF_INET && is_protected_ipv4(bytes)) ||
         (family == AF_INET6 && is_protected_ipv6(bytes)))) {
        return EgressDecision(false, 0, EgressDenyReason::kProtected);
    }
    return EgressDecision(
        apply_default ? default_action_ == CAPSID_EGRESS_ALLOW : true,
        0,
        (apply_default && default_action_ != CAPSID_EGRESS_ALLOW)
            ? EgressDenyReason::kNoMatch
            : EgressDenyReason::kNone);
}

EgressDecision EgressPolicy::decide_resolved_address(
    const struct sockaddr *address,
    socklen_t address_size,
    uint16_t expected_port) const {
    return decide_resolved_address_impl(
        address, address_size, expected_port, true);
}

EgressDecision EgressPolicy::decide_resolved_address_authoritative(
    const struct sockaddr *address,
    socklen_t address_size,
    uint16_t expected_port) const {
    return decide_resolved_address_impl(
        address, address_size, expected_port, false);
}

EgressDecision EgressPolicy::decide_resolved_address_impl(
    const struct sockaddr *address,
    socklen_t address_size,
    uint16_t expected_port,
    bool protect_unmatched) const {
    if (!address || expected_port == 0) {
        return EgressDecision();
    }
    if (address->sa_family == AF_INET &&
        address_size >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
        const sockaddr_in *v4 =
            reinterpret_cast<const sockaddr_in *>(address);
        if (ntohs(v4->sin_port) != expected_port) {
            return EgressDecision();
        }
        return decide_address(
            reinterpret_cast<const uint8_t *>(&v4->sin_addr),
            AF_INET,
            expected_port,
            false,
            protect_unmatched);
    }
    if (address->sa_family == AF_INET6 &&
        address_size >= static_cast<socklen_t>(sizeof(sockaddr_in6))) {
        const sockaddr_in6 *v6 =
            reinterpret_cast<const sockaddr_in6 *>(address);
        if (ntohs(v6->sin6_port) != expected_port) {
            return EgressDecision();
        }
        const uint8_t *bytes =
            reinterpret_cast<const uint8_t *>(&v6->sin6_addr);
        if (is_ipv4_mapped(bytes)) {
            return decide_address(
                bytes + 12, AF_INET, expected_port, false,
                protect_unmatched);
        }
        return decide_address(
            bytes, AF_INET6, expected_port, false, protect_unmatched);
    }
    return EgressDecision();
}

bool EgressPolicy::allows_host(const std::string &host,
                               uint16_t port) const {
    return decide_host(host, port).allowed;
}

bool EgressPolicy::allows_resolved_address(
    const struct sockaddr *address,
    socklen_t address_size,
    uint16_t expected_port) const {
    return decide_resolved_address(
               address, address_size, expected_port)
        .allowed;
}

capsid_permission_state EgressPolicy::query_state() const {
    bool has_allow = default_action_ == CAPSID_EGRESS_ALLOW;
    bool has_deny = default_action_ == CAPSID_EGRESS_DENY;
    for (std::vector<Rule>::const_iterator it = rules_.begin();
         it != rules_.end();
         ++it) {
        has_allow = has_allow || it->action == CAPSID_EGRESS_ALLOW;
        has_deny = has_deny || it->action == CAPSID_EGRESS_DENY;
    }
    if (has_allow && has_deny) {
        return CAPSID_PERMISSION_STATE_PARTIAL;
    }
    return has_allow ? CAPSID_PERMISSION_STATE_GRANTED
                     : CAPSID_PERMISSION_STATE_DENIED;
}

}  // namespace capsid
