#include "host/listener_cors.h"

#include <algorithm>
#include <string_view>

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>

#include "host/request_normalization.h"

namespace capsid::host {

namespace {

std::string ascii_lower(std::string_view text) {
    std::string lower;
    lower.reserve(text.size());
    for (const unsigned char c : text) {
        lower.push_back(
            c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                 : static_cast<char>(c));
    }
    return lower;
}

// Merges a `Vary: Origin` token into the response's Vary set. When the
// listener owns CORS, the final Access-Control-* fields depend on the
// request Origin, so any shared cache must partition on it; an
// App-supplied `Vary: Accept-Encoding` must not be mistaken for an Origin
// token and suppress that partition (cache-poisoning risk otherwise).
void merge_vary_origin(
    std::vector<std::pair<std::string, std::string>>* headers) {
    std::vector<std::string> tokens;
    std::vector<std::pair<std::string, std::string>> filtered;
    filtered.reserve(headers->size() + 1);
    for (const auto& [name, value] : *headers) {
        if (ascii_lower(name) != "vary") {
            filtered.push_back({name, value});
            continue;
        }
        std::size_t begin = 0;
        while (begin <= value.size()) {
            while (begin < value.size() &&
                   (value[begin] == ' ' || value[begin] == '\t')) {
                ++begin;
            }
            const std::size_t end = value.find(',', begin);
            const std::size_t token_end =
                end == std::string::npos ? value.size() : end;
            std::size_t trimmed = token_end;
            while (trimmed > begin &&
                   (value[trimmed - 1] == ' ' || value[trimmed - 1] == '\t')) {
                --trimmed;
            }
            if (trimmed > begin) {
                tokens.push_back(
                    ascii_lower(value.substr(begin, trimmed - begin)));
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }
    bool has_origin = false;
    for (const std::string& token : tokens) {
        has_origin = has_origin || token == "origin";
    }
    if (!has_origin) {
        tokens.push_back("origin");
    }
    std::string joined;
    for (const std::string& token : tokens) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += token;
    }
    filtered.push_back({"Vary", joined});
    *headers = std::move(filtered);
}

}  // namespace

bool valid_cors_origin(const std::string& value) {
    if (value == "*") {
        return true;
    }
    const std::string::size_type scheme_end = value.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::string scheme = value.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    return is_valid_public_authority(
        std::string_view(value).substr(scheme_end + 3));
}

bool valid_cors_method_token(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.' || c == '!' || c == '*' || c == '\'' ||
                        c == '(' || c == ')' || c == '+' || c == ',' ||
                        c == ':' || c == '=' || c == '@' || c == '[' ||
                        c == ']' || c == '~';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool valid_cors_header_token(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (const unsigned char c : value) {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        if (!alnum && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

ListenerCors::ListenerCors(const ListenerCorsConfig& config)
    : config_(config) {
    wildcard_ = config_.allowed_origins.size() == 1 &&
                config_.allowed_origins[0] == "*";
}

CorsDecision ListenerCors::prepare(
    const http::request<http::string_body>& request) {
    origin_seen_ = false;
    origin_allowed_ = false;
    origin_.clear();
    decision_ = CorsDecision::kProceed;

    std::string origin;
    unsigned origin_count = 0;
    for (const auto& field : request.base()) {
        if (field.name() == http::field::origin) {
            ++origin_count;
            if (origin.empty()) {
                origin = std::string(field.value());
            }
        }
    }
    if (origin_count > 1) {
        decision_ = CorsDecision::kBadRequest;
        return decision_;
    }
    origin_seen_ = !origin.empty();
    origin_allowed_ =
        !origin.empty() &&
        (wildcard_ ||
         std::find(config_.allowed_origins.begin(),
                   config_.allowed_origins.end(),
                   origin) != config_.allowed_origins.end());
    if (origin_allowed_) {
        origin_ = origin;
    }

    const std::string_view requested_method =
        request[http::field::access_control_request_method];
    if (request.method() == http::verb::options && !origin.empty() &&
        !requested_method.empty()) {
        // Preflight: match the requested method and each requested header
        // against the config. A reject is 403 WITHOUT any
        // Access-Control-Allow-* field — the browser reports the CORS
        // failure, and no CORS decision leaks into the response.
        bool allowed = origin_allowed_;
        if (allowed) {
            std::string upper(requested_method);
            for (char& c : upper) {
                if (c >= 'a' && c <= 'z') {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            allowed = std::find(config_.allowed_methods.begin(),
                                config_.allowed_methods.end(),
                                upper) != config_.allowed_methods.end();
        }
        if (allowed) {
            const std::string_view requested_headers =
                request[http::field::access_control_request_headers];
            std::size_t start = 0;
            while (start <= requested_headers.size()) {
                const std::size_t comma = requested_headers.find(',', start);
                const std::size_t end =
                    comma == std::string_view::npos
                        ? requested_headers.size()
                        : comma;
                std::string name(requested_headers.substr(start, end - start));
                name.erase(name.find_last_not_of(" \t") == std::string::npos
                               ? 0
                               : name.find_last_not_of(" \t") + 1);
                name.erase(0, name.find_first_not_of(" \t") == std::string::npos
                                  ? name.size()
                                  : name.find_first_not_of(" \t"));
                for (char& c : name) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
                if (!name.empty() &&
                    std::find(config_.allowed_headers.begin(),
                              config_.allowed_headers.end(),
                              name) == config_.allowed_headers.end()) {
                    allowed = false;
                    break;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                start = comma + 1;
            }
        }
        decision_ = allowed ? CorsDecision::kPreflightAllowed
                            : CorsDecision::kPreflightRejected;
    }
    return decision_;
}

void ListenerCors::build_preflight(
    http::response<http::string_body>& response) const {
    if (decision_ == CorsDecision::kPreflightAllowed) {
        response.result(http::status::no_content);
        response.set(http::field::access_control_allow_origin, origin_);
        response.set(http::field::vary, "Origin");
        std::string methods;
        for (const std::string& method : config_.allowed_methods) {
            if (!methods.empty()) {
                methods += ", ";
            }
            methods += method;
        }
        response.set(http::field::access_control_allow_methods, methods);
        std::string headers;
        for (const std::string& header : config_.allowed_headers) {
            if (!headers.empty()) {
                headers += ", ";
            }
            headers += header;
        }
        response.set(http::field::access_control_allow_headers, headers);
        if (config_.max_age_ms > 0) {
            response.set(http::field::access_control_max_age,
                         std::to_string(config_.max_age_ms / 1000));
        }
        response.body() = std::string();
    } else {
        response.result(http::status::forbidden);
        response.set(http::field::content_type, "text/plain");
        response.body() = "CORS preflight rejected";
    }
}

void ListenerCors::filter_headers(
    std::vector<std::pair<std::string, std::string>>* headers) const {
    // Listener-level CORS is authoritative when configured: the App
    // cannot write its own Access-Control-Allow-Origin (that would
    // bypass the Host allow-list), and credentials survive only for an
    // exact allowed origin. A wildcard or a disallowed/absent Origin
    // strips App-owned credentials so wildcard never becomes any-origin
    // credentialed CORS.
    const bool exact_origin =
        origin_allowed_ && origin_seen_ && !wildcard_;
    std::vector<std::pair<std::string, std::string>> cors_filtered;
    cors_filtered.reserve(headers->size() + 2);
    for (const auto& [name, value] : *headers) {
        const std::string lower = ascii_lower(name);
        if (lower == "access-control-allow-origin") {
            continue;  // Host decides this field
        }
        if (lower == "access-control-allow-credentials" && !exact_origin) {
            continue;  // wildcard/disallowed origins never get it
        }
        cors_filtered.push_back({name, value});
    }
    if (origin_allowed_) {
        cors_filtered.push_back(
            {"access-control-allow-origin", origin_});
    }
    merge_vary_origin(&cors_filtered);
    *headers = std::move(cors_filtered);
}

void ListenerCors::stamp(http::response<http::string_body>& response) const {
    if (origin_allowed_) {
        response.set(http::field::access_control_allow_origin, origin_);
        response.set(http::field::vary, "Origin");
    }
}

}  // namespace capsid::host
