// Request normalization: builds the complete worker-observable request
// identity from an already parsed HTTP/1 request.
//
// This boundary validates URL/header semantics, routes the App, and strips
// unsafe fields. It deliberately does not decide HTTP framing or
// reinterpret Content-Length/Transfer-Encoding: Beast is the sole framing
// authority before this function, so Transfer-Encoding is removed only
// because it is hop-by-hop, and duplicate Content-Length fields pass
// through untouched.
//
// v1 accepts only origin-form ASCII targets and ASCII DNS/IPv4-style
// authorities with an optional decimal port in [1, 65535]. Query bytes are
// validated and copied without decode/re-encode. On failure the request is
// empty; on success every returned string is owned.

#include "host/request_normalization.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {
namespace {

using ErrorCode = RequestNormalizationErrorCode;

void set_error(RequestNormalizationError &error,
               ErrorCode code,
               std::string path,
               std::string message) {
    error.code = code;
    error.path = std::move(path);
    error.message = std::move(message);
}

// Hand-written ASCII checks: locale-independent.
bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}
bool ascii_hex(char c) {
    return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool ascii_lower(char c) {
    return c >= 'a' && c <= 'z';
}
char ascii_lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
// RFC 7230 token characters.
bool token_char(char c) {
    if (ascii_digit(c) || ascii_lower(c) || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

// App IDs: [a-z0-9][a-z0-9._-]{0,62}.
bool valid_application_id(std::string_view value) {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    if (!(ascii_lower(value[0]) || ascii_digit(value[0]))) {
        return false;
    }
    for (const char c : value) {
        if (!(ascii_lower(c) || ascii_digit(c) || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

// DNS label: [a-z0-9-]+, no leading or trailing hyphen.
// DNS label: [a-z0-9-]+, at most 63 bytes, no leading or trailing hyphen.
bool valid_label(std::string_view label) {
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-') {
        return false;
    }
    for (const char c : label) {
        if (!(ascii_lower(c) || ascii_digit(c) || c == '-')) {
            return false;
        }
    }
    return true;
}

struct ParsedAuthority {
    std::string host;  // lowercase
    std::optional<std::uint16_t> port;
};

// host[:port]: lowercase DNS labels with an optional decimal port in
// [1, 65535]. Userinfo, bracketed literals, empty/trailing labels,
// oversized labels (>63 bytes) or hostnames (>253 bytes) and empty or
// non-numeric ports are rejected.
bool parse_authority(std::string_view value, ParsedAuthority *out) {
    if (value.empty() || value.size() > kMaxPublicAuthorityBytes) {
        return false;
    }
    const std::size_t colon = value.find(':');
    const std::string_view host_part = value.substr(0, colon);
    const bool has_port = colon != std::string_view::npos;
    const std::string_view port_part =
        has_port ? value.substr(colon + 1) : std::string_view();

    std::string host;
    host.reserve(host_part.size());
    for (const char c : host_part) {
        const char lowered = ascii_lower_char(c);
        if (ascii_lower(lowered) || ascii_digit(lowered) || lowered == '-' ||
            lowered == '.') {
            host.push_back(lowered);
        } else {
            return false;
        }
    }
    // DNS total-length limit applies to the hostname only.
    if (host.size() > 253) {
        return false;
    }
    std::size_t start = 0;
    while (start <= host.size()) {
        const std::size_t dot = host.find('.', start);
        const std::size_t end = dot == std::string::npos ? host.size() : dot;
        if (!valid_label(std::string_view(host).substr(start, end - start))) {
            return false;
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }

    if (has_port) {
        // A colon demands a non-empty decimal port.
        if (port_part.empty() || port_part.size() > 5) {
            return false;
        }
        std::uint32_t port = 0;
        for (const char c : port_part) {
            if (!ascii_digit(c)) {
                return false;
            }
            port = port * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (port == 0 || port > 65535) {
            return false;
        }
        out->port = static_cast<std::uint16_t>(port);
    }
    out->host = std::move(host);
    return true;
}

bool validate_policy(const RequestRoutingPolicy &policy,
                     RequestNormalizationError &error) {
    if (policy.public_scheme != "http" && policy.public_scheme != "https") {
        set_error(error, ErrorCode::kInvalidListenerPolicy,
                  "/listener/publicScheme",
                  "public scheme must be exactly http or https");
        return false;
    }
    // An explicit switch keeps an invalid routing-mode enum from falling
    // into a routing branch (which would also bypass the trust check).
    switch (policy.mode) {
    case RequestRoutingMode::kSubdomain: {
        if (policy.subdomain_suffix.empty() ||
            policy.subdomain_suffix.front() != '.') {
            set_error(error, ErrorCode::kInvalidListenerPolicy,
                      "/listener/routing/suffix",
                      "subdomain suffix must start with a dot");
            return false;
        }
        if (policy.subdomain_suffix.find(':') != std::string_view::npos) {
            set_error(error, ErrorCode::kInvalidListenerPolicy,
                      "/listener/routing/suffix",
                      "subdomain suffix must not contain a port");
            return false;
        }
        std::string suffix;
        suffix.reserve(policy.subdomain_suffix.size() - 1);
        for (const char c : policy.subdomain_suffix.substr(1)) {
            suffix.push_back(ascii_lower_char(c));
        }
        std::size_t start = 0;
        while (start <= suffix.size()) {
            const std::size_t dot = suffix.find('.', start);
            const std::size_t end =
                dot == std::string::npos ? suffix.size() : dot;
            if (!valid_label(
                    std::string_view(suffix).substr(start, end - start))) {
                set_error(error, ErrorCode::kInvalidListenerPolicy,
                          "/listener/routing/suffix",
                          "subdomain suffix is not a valid domain");
                return false;
            }
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        return true;
    }
    case RequestRoutingMode::kPath:
    case RequestRoutingMode::kHeader: {
        ParsedAuthority parsed;
        if (policy.public_authority.empty() ||
            !parse_authority(policy.public_authority, &parsed)) {
            set_error(error, ErrorCode::kInvalidListenerPolicy,
                      "/listener/publicAuthority",
                      "public authority must be host[:port]");
            return false;
        }
        if (policy.mode == RequestRoutingMode::kHeader &&
            !policy.trusted_header_routing) {
            set_error(error, ErrorCode::kUntrustedHeaderRouting,
                      "/listener/routing/mode",
                      "header routing requires a trusted listener");
            return false;
        }
        return true;
    }
    }
    set_error(error, ErrorCode::kInvalidListenerPolicy,
              "/listener/routing/mode", "unknown routing mode");
    return false;
}

// Origin-form target validation. The query is opaque: percent escapes must
// be well-formed, but encoded slashes and dot segments are only rejected in
// the path.
bool validate_target(std::string_view target, RequestNormalizationError &error) {
    if (target.size() > kMaxPublicRequestTargetBytes) {
        set_error(error, ErrorCode::kResourceLimit, "/target",
                  "request target exceeds the size limit");
        return false;
    }
    if (target.empty() || target.front() != '/') {
        set_error(error, ErrorCode::kInvalidTarget, "/target",
                  "only origin-form request targets are accepted");
        return false;
    }
    for (const char c : target) {
        const unsigned char ch = static_cast<unsigned char>(c);
        if (ch >= 0x80 || ch < 0x21 || ch == 0x7f) {
            set_error(error, ErrorCode::kInvalidTarget, "/target",
                      "request target contains a space, control or "
                      "non-ASCII byte");
            return false;
        }
        if (c == '\\' || c == '#') {
            set_error(error, ErrorCode::kInvalidTarget, "/target",
                      "request target contains a forbidden byte");
            return false;
        }
    }
    for (std::size_t index = 0; index < target.size(); ++index) {
        if (target[index] == '%') {
            if (index + 2 >= target.size() ||
                !ascii_hex(target[index + 1]) ||
                !ascii_hex(target[index + 2])) {
                set_error(error, ErrorCode::kInvalidTarget, "/target",
                          "request target has a malformed percent escape");
                return false;
            }
        }
    }

    const std::size_t query_pos = target.find('?');
    const std::string_view path = target.substr(0, query_pos);
    for (std::size_t index = 0; index + 2 < path.size(); ++index) {
        if (path[index] != '%') {
            continue;
        }
        auto hex_value = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        const unsigned char byte = static_cast<unsigned char>(
            (hex_value(path[index + 1]) << 4) | hex_value(path[index + 2]));
        if (byte == '/' || byte == '\\') {
            set_error(error, ErrorCode::kInvalidTarget, "/target",
                      "request path contains an encoded slash");
            return false;
        }
    }
    // Dot segments: raw or percent-encoded "." / ".." in the path.
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size()
                                                                : slash;
        const std::string_view segment = path.substr(start, end - start);
        if (segment == "." || segment == "..") {
            set_error(error, ErrorCode::kInvalidTarget, "/target",
                      "request path contains a dot segment");
            return false;
        }
        std::string decoded;
        decoded.reserve(segment.size());
        for (std::size_t index = 0; index < segment.size(); ++index) {
            if (segment[index] == '%' && index + 2 < segment.size()) {
                auto hex_value = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return c - 'A' + 10;
                };
                decoded.push_back(static_cast<char>(
                    (hex_value(segment[index + 1]) << 4) |
                    hex_value(segment[index + 2])));
                index += 2;
            } else {
                decoded.push_back(segment[index]);
            }
        }
        if (decoded == "." || decoded == "..") {
            set_error(error, ErrorCode::kInvalidTarget, "/target",
                      "request path contains an encoded dot segment");
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

// Fixed hop-by-hop and forwarding field names (lowercase).
bool is_hop_by_hop(std::string_view name) {
    return name == "connection" || name == "keep-alive" ||
           name == "proxy-connection" || name == "te" ||
           name == "trailer" || name == "transfer-encoding" ||
           name == "upgrade";
}
bool is_forwarding(std::string_view name) {
    return name == "forwarded" || name == "x-real-ip" ||
           name.starts_with("x-forwarded-");
}

}  // namespace

bool is_valid_public_authority(std::string_view value) {
    ParsedAuthority parsed;
    return parse_authority(value, &parsed);
}

RequestNormalizationResult normalize_public_request(
    const RequestRoutingPolicy &policy,
    std::string_view target,
    std::span<const PublicRequestHeaderView> headers) {
    RequestNormalizationResult result;

    // Resource limits first: header count and total header bytes.
    if (headers.size() > kMaxPublicRequestHeaderFields) {
        set_error(result.error, ErrorCode::kResourceLimit, "/headers",
                  "too many request header fields");
        return result;
    }
    std::size_t total_bytes = 0;
    for (const PublicRequestHeaderView &header : headers) {
        total_bytes += header.name.size() + header.value.size();
    }
    if (total_bytes > kMaxPublicRequestHeaderBytes) {
        set_error(result.error, ErrorCode::kResourceLimit, "/headers",
                  "request header bytes exceed the limit");
        return result;
    }

    if (!validate_policy(policy, result.error)) {
        return result;
    }
    if (!validate_target(target, result.error)) {
        return result;
    }

    // Header syntax validation and collection. Header-value errors point at
    // the header index; missing/duplicate control fields point at their name.
    std::vector<std::string> lowered_names;
    lowered_names.reserve(headers.size());
    std::optional<std::size_t> host_index;
    std::optional<std::size_t> capsid_app_index;
    std::size_t capsid_app_count = 0;
    std::vector<std::string> connection_nominated;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const PublicRequestHeaderView &header = headers[index];
        if (header.name.empty()) {
            set_error(result.error, ErrorCode::kInvalidHeader,
                      "/headers/" + std::to_string(index),
                      "header name must not be empty");
            return result;
        }
        for (const char c : header.name) {
            if (!token_char(c)) {
                set_error(result.error, ErrorCode::kInvalidHeader,
                          "/headers/" + std::to_string(index),
                          "header name is not a valid token");
                return result;
            }
        }
        for (const unsigned char c : header.value) {
            if (!(c == '\t' || (c >= 0x20 && c <= 0x7e))) {
                set_error(result.error, ErrorCode::kInvalidHeader,
                          "/headers/" + std::to_string(index),
                          "header value contains a forbidden byte");
                return result;
            }
        }
        std::string name;
        name.reserve(header.name.size());
        for (const char c : header.name) {
            name.push_back(ascii_lower_char(c));
        }
        lowered_names.push_back(std::move(name));
        const std::string_view lowered = lowered_names.back();
        if (lowered == "host") {
            if (host_index.has_value()) {
                set_error(result.error, ErrorCode::kInvalidHeader,
                          "/headers/host", "duplicate Host header");
                return result;
            }
            host_index = index;
        } else if (lowered == "capsid-app") {
            // Cardinality is enforced only by header routing; path and
            // subdomain routes strip every Capsid-App field unconditionally.
            if (!capsid_app_index.has_value()) {
                capsid_app_index = index;
            }
            ++capsid_app_count;
        } else if (lowered == "connection") {
            // Parse Connection tokens: comma-separated, non-empty tokens.
            std::size_t start = 0;
            while (start <= header.value.size()) {
                while (start < header.value.size() &&
                       (header.value[start] == ' ' ||
                        header.value[start] == '\t')) {
                    ++start;
                }
                std::size_t end = header.value.find(',', start);
                if (end == std::string_view::npos) {
                    end = header.value.size();
                }
                std::size_t token_end = end;
                while (token_end > start &&
                       (header.value[token_end - 1] == ' ' ||
                        header.value[token_end - 1] == '\t')) {
                    --token_end;
                }
                const std::string_view token =
                    header.value.substr(start, token_end - start);
                if (token.empty()) {
                    set_error(result.error, ErrorCode::kInvalidHeader,
                              "/headers/" + std::to_string(index),
                              "Connection header contains an empty token");
                    return result;
                }
                for (const char c : token) {
                    if (!token_char(c)) {
                        set_error(result.error, ErrorCode::kInvalidHeader,
                                  "/headers/" + std::to_string(index),
                                  "Connection header contains an invalid token");
                        return result;
                    }
                }
                std::string nominated;
                nominated.reserve(token.size());
                for (const char c : token) {
                    nominated.push_back(ascii_lower_char(c));
                }
                connection_nominated.push_back(std::move(nominated));
                if (end == header.value.size()) {
                    break;
                }
                start = end + 1;
            }
        }
    }

    // Exactly one Host is required for every routing mode.
    if (!host_index.has_value()) {
        set_error(result.error, ErrorCode::kInvalidHeader, "/headers/host",
                  "Host header is required");
        return result;
    }
    ParsedAuthority host_authority;
    if (!parse_authority(headers[*host_index].value, &host_authority)) {
        set_error(result.error, ErrorCode::kInvalidAuthority,
                  "/headers/" + std::to_string(*host_index),
                  "Host header is not a valid authority");
        return result;
    }

    std::string application;
    std::string url;
    if (policy.mode == RequestRoutingMode::kSubdomain) {
        // The hostname must end with the (dot-prefixed) suffix and carry
        // exactly one App label before it.
        std::string suffix;
        suffix.reserve(policy.subdomain_suffix.size());
        for (const char c : policy.subdomain_suffix) {
            suffix.push_back(ascii_lower_char(c));
        }
        const std::string_view suffix_domain =
            std::string_view(suffix).substr(1);
        const std::string &host = host_authority.host;
        if (host.size() <= suffix_domain.size() ||
            host.substr(host.size() - suffix_domain.size()) != suffix_domain ||
            host[host.size() - suffix_domain.size() - 1] != '.') {
            set_error(result.error, ErrorCode::kInvalidAuthority,
                      "/headers/" + std::to_string(*host_index),
                      "Host does not match the subdomain suffix");
            return result;
        }
        const std::string_view prefix = std::string_view(host).substr(
            0, host.size() - suffix_domain.size() - 1);
        if (prefix.empty() || prefix.find('.') != std::string_view::npos) {
            set_error(result.error, ErrorCode::kInvalidAuthority,
                      "/headers/" + std::to_string(*host_index),
                      "Host carries an ambiguous App label");
            return result;
        }
        // The extracted App must also satisfy the App ID contract (length
        // and character set); the DNS label grammar is a strict subset.
        if (!valid_application_id(prefix)) {
            set_error(result.error, ErrorCode::kInvalidAuthority,
                      "/headers/" + std::to_string(*host_index),
                      "Host App label exceeds the App ID limits");
            return result;
        }
        application = std::string(prefix);
        url = std::string(policy.public_scheme) + "://" + application +
              suffix;
        if (host_authority.port.has_value()) {
            url += ":" + std::to_string(*host_authority.port);
        }
        url += std::string(target);
    } else {
        // Path and header modes construct the public origin only from the
        // listener policy, never from the request Host.
        std::string authority;
        {
            ParsedAuthority parsed;
            if (!parse_authority(policy.public_authority, &parsed)) {
                set_error(result.error, ErrorCode::kInvalidListenerPolicy,
                          "/listener/publicAuthority",
                          "public authority must be host[:port]");
                return result;
            }
            authority = parsed.host;
            if (parsed.port.has_value()) {
                authority += ":" + std::to_string(*parsed.port);
            }
        }
        if (policy.mode == RequestRoutingMode::kPath) {
            static constexpr std::string_view kPathPrefix = "/@capsid/";
            if (target.substr(0, kPathPrefix.size()) != kPathPrefix) {
                set_error(result.error, ErrorCode::kRouteNotFound, "/target",
                          "path routing requires the /@capsid/{app} prefix");
                return result;
            }
            const std::size_t app_start = kPathPrefix.size();
            const std::size_t app_end =
                target.find_first_of("/?", app_start);
            const std::size_t end = app_end == std::string_view::npos
                                        ? target.size()
                                        : app_end;
            const std::string_view app =
                target.substr(app_start, end - app_start);
            if (app.empty() || !valid_application_id(app)) {
                set_error(result.error, ErrorCode::kRouteNotFound, "/target",
                          "path routing requires a valid App segment");
                return result;
            }
            application = std::string(app);
            std::string rewritten;
            if (end == target.size()) {
                rewritten = "/";
            } else if (target[end] == '/') {
                rewritten = std::string(target.substr(end));
            } else {
                rewritten = "/" + std::string(target.substr(end));
            }
            url = std::string(policy.public_scheme) + "://" + authority +
                  rewritten;
        } else {
            // Header routing: trusted listener, exactly one Capsid-App.
            if (capsid_app_count == 0) {
                set_error(result.error, ErrorCode::kRouteNotFound,
                          "/headers/capsid-app",
                          "header routing requires a Capsid-App header");
                return result;
            }
            if (capsid_app_count > 1) {
                set_error(result.error, ErrorCode::kInvalidHeader,
                          "/headers/capsid-app",
                          "duplicate Capsid-App header");
                return result;
            }
            const std::string_view app_value =
                headers[*capsid_app_index].value;
            if (!valid_application_id(app_value)) {
                set_error(result.error, ErrorCode::kRouteNotFound,
                          "/headers/" + std::to_string(*capsid_app_index),
                          "Capsid-App is not a valid App ID");
                return result;
            }
            application = std::string(app_value);
            url = std::string(policy.public_scheme) + "://" + authority +
                  std::string(target);
        }
    }

    // Output headers: input order, lowercase names, unsafe fields removed.
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const std::string_view name = lowered_names[index];
        bool removed = name == "host" || name == "capsid-app" ||
                       is_hop_by_hop(name) || is_forwarding(name);
        if (!removed) {
            for (const std::string &nominated : connection_nominated) {
                if (name == nominated) {
                    removed = true;
                    break;
                }
            }
        }
        if (removed) {
            continue;
        }
        NormalizedPublicRequestHeader output;
        output.name = std::string(name);
        output.value = std::string(headers[index].value);
        result.request.headers.push_back(std::move(output));
    }

    result.ok = true;
    result.request.application = std::move(application);
    result.request.url = std::move(url);
    return result;
}

}  // namespace capsid::host
