#ifndef CAPSID_HOST_REQUEST_NORMALIZATION_H
#define CAPSID_HOST_REQUEST_NORMALIZATION_H

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

inline constexpr std::size_t kMaxPublicRequestTargetBytes = 16U * 1024U;
inline constexpr std::size_t kMaxPublicRequestHeaderFields = 128U;
// Sum of the original header-name and header-value byte lengths. HTTP framing
// bytes are accounted by the listener/Beast buffer limit, not a second parser.
inline constexpr std::size_t kMaxPublicRequestHeaderBytes = 64U * 1024U;
inline constexpr std::size_t kMaxPublicAuthorityBytes = 259U;

enum class RequestRoutingMode {
    kSubdomain,
    kPath,
    kHeader,
};

struct RequestRoutingPolicy {
    RequestRoutingMode mode = RequestRoutingMode::kPath;
    std::string_view public_scheme;
    // Required only by kSubdomain. It has a leading dot, contains no port,
    // and names the exact DNS suffix (for example, ".apps.example.com").
    std::string_view subdomain_suffix;
    // Required only by kPath/kHeader. It is independent of the request Host.
    std::string_view public_authority;
    // The listener adapter sets this only after its Unix/mTLS/source trust
    // boundary succeeds. kHeader fails closed when it is false.
    bool trusted_header_routing = false;
};

struct PublicRequestHeaderView {
    std::string_view name;
    std::string_view value;
};

struct NormalizedPublicRequestHeader {
    std::string name;
    std::string value;
};

struct NormalizedPublicRequest {
    std::string application;
    std::string url;
    // End-to-end headers in input order. Names are lowercase and values are
    // unchanged. Host, route-control, forwarding, hop-by-hop, and
    // Connection-nominated fields are absent.
    std::vector<NormalizedPublicRequestHeader> headers;
};

enum class RequestNormalizationErrorCode {
    kNone,
    kInvalidListenerPolicy,
    kInvalidTarget,
    kInvalidAuthority,
    kInvalidHeader,
    kResourceLimit,
    kRouteNotFound,
    kUntrustedHeaderRouting,
};

struct RequestNormalizationError {
    RequestNormalizationErrorCode code = RequestNormalizationErrorCode::kNone;
    std::string path;
    std::string message;
};

struct RequestNormalizationResult {
    bool ok = false;
    NormalizedPublicRequest request;
    RequestNormalizationError error;
};

// Validates a public authority (host[:port]) against the M0 grammar used by
// normalize_public_request: lowercase DNS labels with an optional decimal
// port in [1, 65535]; userinfo, bracketed literals, empty/trailing labels,
// oversized labels or hostnames, and empty or non-numeric ports are rejected.
// The CLI calls this before anything is spawned so a malformed
// --public-authority fails the argument phase, not the post-spawn bind phase.
bool is_valid_public_authority(std::string_view value);

// Validates the policy the listener adapter would route with, without doing
// any normalization. error is optional (null is allowed when only the boolean
// is wanted). The adapter calls this before binding so a malformed policy
// fails startup closed.
bool is_valid_routing_policy(const RequestRoutingPolicy& policy,
                             RequestNormalizationError* error);

// Builds the complete worker-observable request identity from an already
// parsed HTTP/1 request. This function validates URL/header semantics, routes
// the App, and strips unsafe fields; it deliberately does not decide HTTP
// framing or reinterpret Content-Length/Transfer-Encoding. Beast is the sole
// framing authority before this boundary.
//
// v1 accepts only origin-form ASCII targets and ASCII DNS/IPv4-style
// authorities with an optional decimal port in [1, 65535]. It rejects
// userinfo, bracketed IP literals, empty/trailing DNS labels, raw or encoded
// slash/backslash in routed path segments, and raw/encoded dot segments.
// Query bytes are validated and copied without decode/re-encode.
//
// Exactly one Host header is required for every routing mode. Header routing
// additionally requires exactly one Capsid-App header and a trusted listener.
// On failure request is empty; on success all returned strings are owned.
RequestNormalizationResult normalize_public_request(
    const RequestRoutingPolicy& policy,
    std::string_view target,
    std::span<const PublicRequestHeaderView> headers);

}  // namespace capsid::host

#endif
