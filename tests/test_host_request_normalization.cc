#include "host/request_normalization.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using capsid::host::PublicRequestHeaderView;
using capsid::host::RequestNormalizationErrorCode;
using capsid::host::RequestNormalizationResult;
using capsid::host::RequestRoutingMode;
using capsid::host::RequestRoutingPolicy;
using capsid::host::kMaxPublicRequestHeaderBytes;
using capsid::host::kMaxPublicRequestHeaderFields;
using capsid::host::kMaxPublicRequestTargetBytes;
using capsid::host::normalize_public_request;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-request-normalization: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

RequestRoutingPolicy path_policy() {
    return RequestRoutingPolicy{
        RequestRoutingMode::kPath, "https", {}, "Public.Example.Com:00443",
        false};
}

RequestRoutingPolicy header_policy(bool trusted = true) {
    return RequestRoutingPolicy{
        RequestRoutingMode::kHeader, "https", {}, "Api.Example.Com:00443",
        trusted};
}

RequestRoutingPolicy subdomain_policy() {
    return RequestRoutingPolicy{RequestRoutingMode::kSubdomain,
                                "https",
                                ".Apps.Example.Com",
                                {},
                                false};
}

RequestNormalizationResult normalize(
    const RequestRoutingPolicy& policy,
    std::string_view target,
    const std::vector<PublicRequestHeaderView>& headers) {
    return normalize_public_request(policy, target, headers);
}

void require_success(const RequestNormalizationResult& result,
                     std::string_view application,
                     std::string_view url,
                     std::string_view label) {
    require(result.ok, std::string(label) + " was rejected at '" +
                           result.error.path + "': " + result.error.message);
    require(result.request.application == application,
            std::string(label) + " selected App '" +
                result.request.application + "'");
    require(result.request.url == url,
            std::string(label) + " produced URL '" + result.request.url +
                "'");
    require(result.error.code == RequestNormalizationErrorCode::kNone &&
                result.error.path.empty() && result.error.message.empty(),
            std::string(label) + " succeeded with a stale error");
}

void require_error(const RequestNormalizationResult& result,
                   RequestNormalizationErrorCode code,
                   std::string_view path,
                   std::string_view label) {
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == code,
            std::string(label) + " returned the wrong error code");
    require(result.error.path == path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + std::string(path) + "'");
    require(!result.error.message.empty(),
            std::string(label) + " returned no diagnostic");
    require(result.request.application.empty() && result.request.url.empty() &&
                result.request.headers.empty(),
            std::string(label) + " partially published a failed request");
}

void test_forwarded_headers_never_change_subdomain_url() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "Orders.Apps.Example.Com:08443"},
        {"Forwarded", "proto=http;host=attacker.invalid"},
        {"X-Forwarded-Proto", "http"},
        {"x-forwarded-host", "attacker.invalid"},
        {"X-Real-IP", "203.0.113.8"},
        {"X-Forwardedness", "end-to-end"},
        {"X-Trace", "keep-exactly"},
    };
    const RequestNormalizationResult result =
        normalize(subdomain_policy(), "/api/orders?next=%2Fcart", headers);
    require_success(result,
                    "orders",
                    "https://orders.apps.example.com:8443/api/orders?next=%2Fcart",
                    "subdomain route with forged forwarding headers");
    require(result.request.headers.size() == 2,
            "forwarding cleanup retained or removed the wrong fields");
    require(result.request.headers[0].name == "x-forwardedness" &&
                result.request.headers[0].value == "end-to-end" &&
                result.request.headers[1].name == "x-trace" &&
                result.request.headers[1].value == "keep-exactly",
            "forwarding cleanup changed order, names, or values");
}

void test_path_rewrite_table_and_fixed_origin() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "attacker.invalid:1234"},
        {"Capsid-App", "must-not-route"},
    };
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"/@capsid/orders", "/"},
        {"/@capsid/orders/", "/"},
        {"/@capsid/orders?x=1", "/?x=1"},
        {"/@capsid/orders/api?x=1", "/api?x=1"},
    };
    for (const auto& [target, rewritten] : cases) {
        const RequestNormalizationResult result =
            normalize(path_policy(), target, headers);
        require_success(result,
                        "orders",
                        "https://public.example.com:443" +
                            std::string(rewritten),
                        std::string("path rewrite ") + std::string(target));
        require(result.request.headers.empty(),
                "path route exposed Host or Capsid-App to the worker");
    }
}

void test_trusted_header_route_uses_only_capsid_app() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "attacker.invalid"},
        {"X-Forwarded-Host", "other.invalid"},
        {"Capsid-App", "orders.eu"},
        {"Accept", "application/json"},
    };
    const RequestNormalizationResult result =
        normalize(header_policy(), "/api/orders?x=1", headers);
    require_success(result,
                    "orders.eu",
                    "https://api.example.com:443/api/orders?x=1",
                    "trusted header route");
    require(result.request.headers.size() == 1 &&
                result.request.headers[0].name == "accept" &&
                result.request.headers[0].value == "application/json",
            "header route exposed a control header or changed end-to-end data");
}

void test_target_syntax_fails_closed() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "client.example"}, {"Capsid-App", "orders"}};
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"https://example.com/a", "absolute-form"},
        {"example.com:443", "authority-form"},
        {"*", "asterisk-form"},
        {"/bad%", "short percent escape"},
        {"/bad%2x", "non-hex percent escape"},
        {"/bad\\path", "raw backslash"},
        {"/safe/%2fadmin", "encoded slash"},
        {"/safe/%5Cadmin", "encoded backslash"},
        {"/safe/./admin", "raw dot segment"},
        {"/safe/%2e/admin", "encoded dot segment"},
        {"/safe/%2E%2e/admin", "encoded double-dot segment"},
        {"/safe#fragment", "fragment"},
        {"/bad path", "raw space"},
        {"/safe\npath", "control byte"},
        {"/caf\xC3\xA9", "non-ASCII target"},
    };
    for (const auto& [target, label] : cases) {
        require_error(normalize(header_policy(), target, headers),
                      RequestNormalizationErrorCode::kInvalidTarget,
                      "/target",
                      label);
    }

    std::string at_limit(kMaxPublicRequestTargetBytes, 'a');
    at_limit.front() = '/';
    require_success(normalize(header_policy(), at_limit, headers),
                    "orders",
                    "https://api.example.com:443" + at_limit,
                    "target at byte limit");
    at_limit.push_back('a');
    require_error(normalize(header_policy(), at_limit, headers),
                  RequestNormalizationErrorCode::kResourceLimit,
                  "/target",
                  "target over byte limit");
}

void test_path_route_rejects_ambiguous_or_missing_app_segment() {
    const std::vector<PublicRequestHeaderView> headers{{"Host", "public.example"}};
    struct Case {
        std::string_view target;
        RequestNormalizationErrorCode code;
        std::string_view label;
    };
    const Case cases[] = {
        {"/", RequestNormalizationErrorCode::kRouteNotFound,
         "missing route prefix"},
        {"/@capsid", RequestNormalizationErrorCode::kRouteNotFound,
         "missing route slash"},
        {"/@capsid/", RequestNormalizationErrorCode::kRouteNotFound,
         "empty App segment"},
        {"/@capsid/Orders/api", RequestNormalizationErrorCode::kRouteNotFound,
         "uppercase App"},
        {"/@capsid/orders%2fadmin/api",
         RequestNormalizationErrorCode::kInvalidTarget,
         "encoded App separator"},
        {"/@capsid/../api", RequestNormalizationErrorCode::kInvalidTarget,
         "dot App segment"},
    };
    for (const Case& test_case : cases) {
        require_error(
            normalize(path_policy(), test_case.target, headers),
            test_case.code,
            "/target",
            test_case.label);
    }
}

void test_authority_and_suffix_validation() {
    const std::pair<std::string_view, std::string_view> bad_hosts[] = {
        {"apps.example.com", "suffix without App label"},
        {"blue.orders.apps.example.com", "more than one App label"},
        {"orders.apps.example.com.evil", "suffix boundary bypass"},
        {"user@orders.apps.example.com", "userinfo"},
        {"orders.apps.example.com:0", "zero port"},
        {"orders.apps.example.com:65536", "oversized port"},
        {"orders.apps.example.com:", "empty port"},
        {"orders.apps.example.com.", "trailing dot"},
        {"[::1]", "bracketed IPv6 literal"},
        {"-orders.apps.example.com", "leading label hyphen"},
    };
    for (const auto& [host, label] : bad_hosts) {
        const std::vector<PublicRequestHeaderView> headers{{"Host", host}};
        require_error(normalize(subdomain_policy(), "/api", headers),
                      RequestNormalizationErrorCode::kInvalidAuthority,
                      "/headers/0",
                      label);
    }

    require_error(normalize(subdomain_policy(), "/api", {}),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/host",
                  "missing Host");
    const std::vector<PublicRequestHeaderView> duplicate_host{
        {"Host", "orders.apps.example.com"},
        {"host", "orders.apps.example.com"},
    };
    require_error(normalize(subdomain_policy(), "/api", duplicate_host),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/host",
                  "duplicate Host");

    RequestRoutingPolicy bad_suffix = subdomain_policy();
    bad_suffix.subdomain_suffix = "apps.example.com";
    const std::vector<PublicRequestHeaderView> valid_host{
        {"Host", "orders.apps.example.com"}};
    require_error(normalize(bad_suffix, "/api", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/routing/suffix",
                  "suffix without leading dot");

    const std::string long_label(64, 'a');
    RequestRoutingPolicy long_suffix = subdomain_policy();
    const std::string long_suffix_storage = "." + long_label + ".example.com";
    long_suffix.subdomain_suffix = long_suffix_storage;
    require_error(normalize(long_suffix, "/api", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/routing/suffix",
                  "oversized DNS suffix label");

    const std::string long_app_host =
        long_label + ".apps.example.com";
    const std::vector<PublicRequestHeaderView> long_app_headers{
        {"Host", long_app_host}};
    require_error(normalize(subdomain_policy(), "/api", long_app_headers),
                  RequestNormalizationErrorCode::kInvalidAuthority,
                  "/headers/0",
                  "oversized subdomain App label");

    RequestRoutingPolicy bad_fixed = path_policy();
    bad_fixed.public_authority = "https://public.example.com";
    require_error(normalize(bad_fixed, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/publicAuthority",
                  "absolute fixed authority");

    RequestRoutingPolicy empty_fixed_port = path_policy();
    empty_fixed_port.public_authority = "public.example.com:";
    require_error(normalize(empty_fixed_port, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/publicAuthority",
                  "fixed authority with empty port");

    RequestRoutingPolicy long_fixed_label = path_policy();
    const std::string long_fixed_storage = long_label + ".example.com";
    long_fixed_label.public_authority = long_fixed_storage;
    require_error(normalize(long_fixed_label, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/publicAuthority",
                  "fixed authority with oversized DNS label");

    const std::string label63(63, 'b');
    const std::string label61(61, 'c');
    const std::string label62(62, 'c');
    const std::string host253 =
        label63 + "." + label63 + "." + label63 + "." + label61;
    const std::string host254 =
        label63 + "." + label63 + "." + label63 + "." + label62;
    require(host253.size() == 253 && host254.size() == 254,
            "DNS total-length fixtures are malformed");
    RequestRoutingPolicy max_fixed = path_policy();
    const std::string max_fixed_storage = host253 + ":65535";
    max_fixed.public_authority = max_fixed_storage;
    require_success(normalize(max_fixed, "/@capsid/orders", valid_host),
                    "orders",
                    "https://" + max_fixed_storage + "/",
                    "fixed authority at DNS and port limit");
    RequestRoutingPolicy overlong_fixed = path_policy();
    overlong_fixed.public_authority = host254;
    require_error(normalize(overlong_fixed, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/publicAuthority",
                  "fixed authority over DNS host limit");

    RequestRoutingPolicy bad_scheme = path_policy();
    bad_scheme.public_scheme = "HTTPS";
    require_error(normalize(bad_scheme, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/publicScheme",
                  "non-contract scheme");

    RequestRoutingPolicy bad_mode = path_policy();
    bad_mode.mode = static_cast<RequestRoutingMode>(99);
    require_error(normalize(bad_mode, "/@capsid/orders", valid_host),
                  RequestNormalizationErrorCode::kInvalidListenerPolicy,
                  "/listener/routing/mode",
                  "unknown routing mode enum");
}

void test_non_header_routes_ignore_and_strip_capsid_app() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "client.example"},
        {"Capsid-App", "attacker"},
        {"capsid-app", "other"},
        {"X-Trace", "keep"},
    };
    const RequestNormalizationResult result =
        normalize(path_policy(), "/@capsid/orders/api", headers);
    require_success(result,
                    "orders",
                    "https://public.example.com:443/api",
                    "path route with spoofed control headers");
    require(result.request.headers.size() == 1 &&
                result.request.headers[0].name == "x-trace" &&
                result.request.headers[0].value == "keep",
            "non-header route exposed or interpreted Capsid-App");
}

void test_hop_by_hop_and_connection_nominated_headers_are_removed() {
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"Connection", " X-Remove, x-second "},
        {"X-Remove", "one"},
        {"x-second", "two"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Trailer", "Digest"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
        {"Forwarded", "for=unknown"},
        {"X-Forwarded-For", "203.0.113.2"},
        {"X-Real-IP", "203.0.113.2"},
        {"Accept", "text/plain"},
        {"X-Forwardedness", "retain"},
    };
    const RequestNormalizationResult result =
        normalize(header_policy(), "/api", headers);
    require_success(result,
                    "orders",
                    "https://api.example.com:443/api",
                    "hop-by-hop cleaning");
    require(result.request.headers.size() == 2 &&
                result.request.headers[0].name == "accept" &&
                result.request.headers[0].value == "text/plain" &&
                result.request.headers[1].name == "x-forwardedness" &&
                result.request.headers[1].value == "retain",
            "hop-by-hop cleaning did not preserve exact end-to-end order");
}

void test_framing_is_not_reinterpreted_by_normalization() {
    // Beast is the sole framing authority before this pure boundary. This
    // deliberately synthetic input proves the normalizer does not grow a
    // competing CL/TE decision table: end-to-end Content-Length fields remain
    // in order, while Transfer-Encoding is removed only because it is
    // hop-by-hop.
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"Content-Length", "7"},
        {"content-length", "9"},
        {"Transfer-Encoding", "chunked"},
    };
    const RequestNormalizationResult result =
        normalize(header_policy(), "/api", headers);
    require_success(result,
                    "orders",
                    "https://api.example.com:443/api",
                    "already-framed header snapshot");
    require(result.request.headers.size() == 2 &&
                result.request.headers[0].name == "content-length" &&
                result.request.headers[0].value == "7" &&
                result.request.headers[1].name == "content-length" &&
                result.request.headers[1].value == "9",
            "normalizer reinterpreted framing instead of only cleaning headers");
}

void test_header_routing_trust_and_cardinality() {
    const std::vector<PublicRequestHeaderView> valid{
        {"Host", "client.example"}, {"Capsid-App", "orders"}};
    require_error(normalize(header_policy(false), "/api", valid),
                  RequestNormalizationErrorCode::kUntrustedHeaderRouting,
                  "/listener/routing/mode",
                  "untrusted header route");

    const std::vector<PublicRequestHeaderView> missing{{"Host", "client.example"}};
    require_error(normalize(header_policy(), "/api", missing),
                  RequestNormalizationErrorCode::kRouteNotFound,
                  "/headers/capsid-app",
                  "missing Capsid-App");
    const std::vector<PublicRequestHeaderView> duplicate{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"capsid-app", "billing"},
    };
    require_error(normalize(header_policy(), "/api", duplicate),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/capsid-app",
                  "duplicate Capsid-App");
    const std::vector<PublicRequestHeaderView> invalid_app{
        {"Host", "client.example"}, {"Capsid-App", "Orders"}};
    require_error(normalize(header_policy(), "/api", invalid_app),
                  RequestNormalizationErrorCode::kRouteNotFound,
                  "/headers/1",
                  "invalid Capsid-App");
}

void test_header_syntax_and_resource_limits() {
    const std::vector<PublicRequestHeaderView> empty_name{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"", "x"},
    };
    require_error(normalize(header_policy(), "/api", empty_name),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "empty header name");
    const std::vector<PublicRequestHeaderView> bad_name{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"Bad Name", "x"},
    };
    require_error(normalize(header_policy(), "/api", bad_name),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "header name with a space");
    const std::vector<PublicRequestHeaderView> bad_value{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"X-Test", "one\r\ntwo"},
    };
    require_error(normalize(header_policy(), "/api", bad_value),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "header value with CRLF");
    const std::string nul_value("one\0two", 7);
    const std::vector<PublicRequestHeaderView> bad_nul_value{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"X-Test", nul_value},
    };
    require_error(normalize(header_policy(), "/api", bad_nul_value),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "header value with NUL");
    const std::string obs_text(1, static_cast<char>(0x80));
    const std::vector<PublicRequestHeaderView> bad_obs_text{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"X-Test", obs_text},
    };
    require_error(normalize(header_policy(), "/api", bad_obs_text),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "header value with obs-text");
    const std::vector<PublicRequestHeaderView> bad_connection{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"Connection", "x-one,,x-two"},
    };
    require_error(normalize(header_policy(), "/api", bad_connection),
                  RequestNormalizationErrorCode::kInvalidHeader,
                  "/headers/2",
                  "empty Connection token");

    std::vector<std::string> names(kMaxPublicRequestHeaderFields - 2U);
    std::vector<PublicRequestHeaderView> at_count{
        {"Host", "client.example"}, {"Capsid-App", "orders"}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        names[i] = "x-" + std::to_string(i);
        at_count.push_back({names[i], "v"});
    }
    const RequestNormalizationResult count_ok =
        normalize(header_policy(), "/api", at_count);
    require(count_ok.ok, "exact header-count limit was rejected");
    names.push_back("x-over");
    at_count.push_back({names.back(), "v"});
    require_error(normalize(header_policy(), "/api", at_count),
                  RequestNormalizationErrorCode::kResourceLimit,
                  "/headers",
                  "header count over limit");

    constexpr std::size_t kControlBytes =
        std::string_view("Host").size() +
        std::string_view("client.example").size() +
        std::string_view("Capsid-App").size() +
        std::string_view("orders").size() + std::string_view("X-Pad").size();
    std::string padding(kMaxPublicRequestHeaderBytes - kControlBytes, 'a');
    std::vector<PublicRequestHeaderView> at_bytes{
        {"Host", "client.example"},
        {"Capsid-App", "orders"},
        {"X-Pad", padding},
    };
    const RequestNormalizationResult bytes_ok =
        normalize(header_policy(), "/api", at_bytes);
    require(bytes_ok.ok, "exact header-byte limit was rejected");
    padding.push_back('a');
    at_bytes[2].value = padding;
    require_error(normalize(header_policy(), "/api", at_bytes),
                  RequestNormalizationErrorCode::kResourceLimit,
                  "/headers",
                  "header bytes over limit");
}

void test_successful_result_owns_all_strings() {
    std::string target = "/api?x=1";
    std::string host = "client.example";
    std::string app = "orders";
    std::string name = "X-Trace";
    std::string value = "original";
    const std::vector<PublicRequestHeaderView> headers{
        {"Host", host}, {"Capsid-App", app}, {name, value}};
    const RequestNormalizationResult result =
        normalize(header_policy(), target, headers);
    require_success(result,
                    "orders",
                    "https://api.example.com:443/api?x=1",
                    "owning normalized request");

    target.assign(target.size(), 'z');
    host.assign(host.size(), 'z');
    app.assign(app.size(), 'z');
    name.assign(name.size(), 'z');
    value.assign(value.size(), 'z');
    require(result.request.application == "orders" &&
                result.request.url ==
                    "https://api.example.com:443/api?x=1" &&
                result.request.headers.size() == 1 &&
                result.request.headers[0].name == "x-trace" &&
                result.request.headers[0].value == "original",
            "normalized request retained caller-owned storage");
}

}  // namespace

int main() {
    test_forwarded_headers_never_change_subdomain_url();
    test_path_rewrite_table_and_fixed_origin();
    test_trusted_header_route_uses_only_capsid_app();
    test_target_syntax_fails_closed();
    test_path_route_rejects_ambiguous_or_missing_app_segment();
    test_authority_and_suffix_validation();
    test_non_header_routes_ignore_and_strip_capsid_app();
    test_hop_by_hop_and_connection_nominated_headers_are_removed();
    test_framing_is_not_reinterpreted_by_normalization();
    test_header_routing_trust_and_cardinality();
    test_header_syntax_and_resource_limits();
    test_successful_result_owns_all_strings();
    return 0;
}
