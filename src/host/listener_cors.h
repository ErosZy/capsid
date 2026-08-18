#pragma once
// Listener-level CORS: the Host owns Access-Control-* at the edge (a
// trust boundary, like `trusted`). One shared decision engine backs the
// managed listener and the single-worker/static-pool data plane so both
// edges enforce the identical grammar and wire behavior: browser
// preflights are answered before routing, simple responses get the
// matching Access-Control-Allow-Origin stamped, and App-supplied CORS
// fields are stripped (the App can never widen the Host allow-list).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>

#include "host/host_config_model.h"

namespace capsid::host {

// CORS value grammars (the same rules the managed host.json applies): an
// allowed origin is "*" or an exact "scheme://host[:port]" (http/https
// only, portless = the default port); method tokens are RFC 7230 tchar;
// header names are lowercased HTTP field names. Empty values are never
// valid.
bool valid_cors_origin(const std::string& value);
bool valid_cors_method_token(const std::string& value);
bool valid_cors_header_token(const std::string& value);

// Classification of one request under a configured CORS policy.
enum class CorsDecision {
    kProceed,           // not a preflight; origin state recorded
    kBadRequest,        // duplicate Origin header (caller sends 400)
    kPreflightAllowed,  // OPTIONS preflight passes (caller sends 204)
    kPreflightRejected  // OPTIONS preflight fails (caller sends 403)
};

// Per-request CORS engine. One instance per request classification; the
// session keeps it alive while the request is in flight so the response
// paths (worker response filtering, Host-synthesized errors) can stamp or
// strip exactly the fields the decision produced.
class ListenerCors {
  public:
    explicit ListenerCors(const ListenerCorsConfig& config);

    // Classifies the request and records the decision state used by
    // stamp()/filter_headers(). kBadRequest/kPreflight* short-circuit the
    // request: the caller must answer without routing.
    CorsDecision prepare(
        const http::request<http::string_body>& request);

    // Fills the short-circuit response for a kPreflight* decision: 204
    // with the full allow set for an allowed preflight, 403 text/plain
    // with NO Access-Control-Allow-* fields for a rejected one. The
    // caller sets version/keep_alive and writes the response.
    void build_preflight(
        http::response<http::string_body>& response) const;

    // Applies Host-authoritative CORS to the worker's response headers:
    // strips App-owned Access-Control-Allow-Origin, gates
    // Access-Control-Allow-Credentials to an exact allowed origin, stamps
    // the matching ACAO, and merges a Vary: Origin token.
    void filter_headers(
        std::vector<std::pair<std::string, std::string>>* headers) const;

    // Stamps the recorded ACAO (+ Vary: Origin) on a Host-synthesized
    // response (send_simple paths) when the Origin was allowed.
    void stamp(http::response<http::string_body>& response) const;

  private:
    const ListenerCorsConfig& config_;
    bool origin_seen_ = false;
    bool wildcard_ = false;
    bool origin_allowed_ = false;
    std::string origin_;
    CorsDecision decision_ = CorsDecision::kProceed;
};

}  // namespace capsid::host
