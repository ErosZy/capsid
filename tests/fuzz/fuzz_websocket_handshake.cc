// Handshake boundaries from RFC 6455 sections 4.2.1 and 4.2.2.
// The actual host request gate and response builder are linked, alongside
// the custom codec. OpenSSL supplies an independent SHA-1 oracle only.
#include "host/request_normalization.h"
#include "host/request_header_budget.h"
#include "host/ws_codec.h"
#include "host/websocket_session.h"

#include <openssl/evp.h>
#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>
#include <boost/asio/system_executor.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
namespace host = capsid::host;
namespace ws = capsid::host::ws;
using Header = std::pair<std::string, std::string>;
using Headers = std::vector<Header>;
using Gate = host::WebSocketUpgradeCheck;
void invariant(bool value, int line) {
    if (!value) { std::fprintf(stderr, "HANDSHAKE_INVARIANT line=%d\n", line); std::abort(); }
}
#define check(value) invariant((value), __LINE__)

std::string base64(const uint8_t* data, size_t size) {
    const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const size_t remain = size - i;
        const uint32_t bits = uint32_t(data[i]) << 16 |
            (remain > 1 ? uint32_t(data[i+1]) << 8 : 0) | (remain > 2 ? data[i+2] : 0);
        out += table[(bits >> 18) & 63]; out += table[(bits >> 12) & 63];
        out += remain > 1 ? table[(bits >> 6) & 63] : '=';
        out += remain > 2 ? table[bits & 63] : '=';
    }
    return out;
}
std::string accept(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
    check(EVP_Digest(input.data(), input.size(), digest, &length, EVP_sha1(), nullptr) == 1);
    check(length == 20);
    return base64(digest, length);
}
std::vector<host::PublicRequestHeaderView> views(const Headers& headers) {
    std::vector<host::PublicRequestHeaderView> result;
    for (const auto& h : headers) result.push_back({h.first, h.second});
    return result;
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    return value;
}
bool token(const std::string& value, const std::string& wanted) {
    size_t offset = 0;
    while (offset <= value.size()) {
        const auto end = value.find(',', offset);
        if (lower(trim(value.substr(offset, end == std::string::npos ? end : end-offset))) == wanted) return true;
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    return false;
}
Headers canonical(const std::string& key) {
    return {{"Host", "fuzz.test"}, {"Upgrade", "websocket"}, {"Connection", "Upgrade"},
            {"Sec-WebSocket-Key", key}, {"Sec-WebSocket-Version", "13"}};
}

void necessary(const Headers& headers, Gate result, const std::string& method, unsigned version) {
    bool upgrade = false, connection = false;
    unsigned keys = 0, versions = 0;
    std::string key, protocol_version;
    for (const auto& h : headers) {
        const auto name = lower(h.first);
        if (name == "upgrade") upgrade |= token(h.second, "websocket");
        if (name == "connection") connection |= token(h.second, "upgrade");
        if (name == "sec-websocket-key") { ++keys; key = trim(h.second); }
        if (name == "sec-websocket-version") { ++versions; protocol_version = trim(h.second); }
    }
    if (result == Gate::kNone) check(!upgrade);
    if (!upgrade) check(result == Gate::kNone);
    if (result == Gate::kValid) {
        check(upgrade && connection && method == "GET" && version >= 11 && keys == 1 && versions == 1);
        check(protocol_version == "13" && key.size() == 24);
        std::array<unsigned char, 32> decoded{};
        const int bytes = EVP_DecodeBlock(decoded.data(),
            reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()));
        check(bytes == 18 && key.substr(22) == "==");  // exactly 16 decoded octets
    }
}

struct HttpObserved {
    bool complete = false;
    int error = 0;
    size_t consumed = 0;
    unsigned version = 0;
    std::string method, target, body;
    Headers headers;
    Gate gate = Gate::kNone;
    bool operator==(const HttpObserved& other) const {
        return complete == other.complete && error == other.error && consumed == other.consumed &&
            version == other.version && method == other.method && target == other.target &&
            body == other.body && headers == other.headers && gate == other.gate;
    }
};

// Feed the actual Beast composed read implementation. A parser can make an
// internal state transition without consuming bytes; reproducing that loop
// manually incorrectly treated this documented API behavior as a failure.
class InputStream {
    const std::string& input_;
    bool fragmented_;
    size_t offset_ = 0;
public:
    using executor_type = boost::asio::system_executor;
    executor_type get_executor() noexcept { return {}; }
    InputStream(const std::string& input, bool fragmented, size_t initial = 0)
        : input_(input), fragmented_(fragmented), offset_(initial) {}
    template<class Buffers>
    size_t read_some(const Buffers& buffers, boost::system::error_code& error) {
        if (offset_ == input_.size()) {
            error = boost::asio::error::eof;
            return 0;
        }
        size_t count = input_.size() - offset_;
        if (fragmented_) count = std::min(count, offset_ < 20 ?
            1 + offset_ % 3 : 257 + static_cast<unsigned char>(input_[offset_]) * 29);
        const size_t copied = boost::asio::buffer_copy(buffers,
            boost::asio::buffer(input_.data() + offset_, count));
        check(copied != 0 && copied <= count);
        offset_ += copied;
        error.clear();
        return copied;
    }
    template<class Buffers>
    size_t read_some(const Buffers& buffers) {
        boost::system::error_code error;
        const size_t copied = read_some(buffers, error);
        if (error) throw boost::system::system_error(error);
        return copied;
    }
};

HttpObserved http_request(const std::string& input, bool fragmented, size_t initial = 0) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    // Same parser/body type and 16 MiB body limit as Session::read_request.
    // This supplies byte reads only; no socket admission or Worker is claimed.
    http::request_parser<http::string_body> parser;
    parser.body_limit(16u * 1024u * 1024u);
    beast::flat_buffer buffer(131072);
    beast::error_code error;
    check(initial <= input.size());
    boost::asio::buffer_copy(buffer.prepare(initial), boost::asio::buffer(input.data(), initial));
    buffer.commit(initial);
    InputStream input_stream(input, fragmented, initial);
    host::HeaderLimitedReadStream<InputStream> stream(input_stream, 8192);
    HttpObserved out;
    if (!stream.observe_initial_buffer(buffer.data())) error = http::error::header_limit;
    else out.consumed = http::read(stream, buffer, parser, error);
    out.complete = parser.is_done() && !error;
    out.error = error.value();
    if (out.complete) {
        const auto& request = parser.get();
        out.version = request.version();
        out.method.assign(request.method_string().data(), request.method_string().size());
        out.target.assign(request.target().data(), request.target().size());
        out.body = request.body();
        for (const auto& h : request.base()) out.headers.push_back({
            std::string(h.name_string().data(), h.name_string().size()),
            std::string(h.value().data(), h.value().size())});
        out.gate = host::check_websocket_upgrade(views(out.headers), out.method, out.version);
        necessary(out.headers, out.gate, out.method, out.version);
    }
    return out;
}

void http(const uint8_t* data, size_t size, const std::string& key) {
    const std::string raw = size ? std::string(reinterpret_cast<const char*>(data), size) : "";
    const auto a = http_request(raw, false), b = http_request(raw, true);
    // Multiple invalid fields can yield different first diagnostics with
    // different read boundaries. Both reject; completed requests are exact.
    check(a.complete == b.complete);
    if (a.complete) check(a == b);
    else check(a.error != 0 && b.error != 0);
    // Also cover Beast read-ahead from the preceding keep-alive request.
    const auto buffered = http_request(raw, true, raw.size());
    const auto partial = http_request(raw, false, std::min<size_t>(23, raw.size()));
    check(buffered.complete == a.complete && partial.complete == a.complete);
    if (a.complete) check(buffered == a && partial == a);
    else check(buffered.error != 0 && partial.error != 0);
    std::string valid = "GET /@capsid/fuzz/ws HTTP/1.1\r\n";
    for (const auto& h : canonical(key)) valid += h.first + ": " + h.second + "\r\n";
    valid += "\r\n";
    const auto c = http_request(valid, false), d = http_request(valid, true);
    check(c == d && c.complete && c.gate == Gate::kValid && c.body.empty() && c.consumed == valid.size());
    // Retain bodyful requests at this parsing boundary; the actual listener's
    // independent bodyless-admission test remains mandatory.
    valid.insert(valid.size()-2, "Content-Length: 1\r\n"); valid += 'x';
    const auto e = http_request(valid, false), f = http_request(valid, true);
    check(e == f && e.complete && e.gate == Gate::kValid && e.body == "x");
}

void request(const uint8_t* data, size_t size, const std::string& key) {
    Headers input = canonical(key);
    std::string method = "GET"; unsigned version = 11;
    Gate wanted = Gate::kValid; unsigned status = 0;
    switch (size ? data[0] % 12 : 0) {
    case 0: break;
    case 1: input.erase(input.begin()+1); input[2].second = "not a websocket key"; wanted = Gate::kNone; break;
    case 2: input[2].second = "keep-alive"; wanted = Gate::kBadHandshake; status = 400; break;
    case 3: input.erase(input.begin()+3); wanted = Gate::kBadKey; status = 400; break;
    case 4: input.push_back({"SEC-WEBSOCKET-KEY", key}); wanted = Gate::kBadKey; status = 400; break;
    case 5: input[4].second = "8"; wanted = Gate::kUnsupportedVersion; status = 426; break;
    case 6: input.push_back({"Sec-WebSocket-Version", "13"}); wanted = Gate::kUnsupportedVersion; status = 426; break;
    case 7: method = "POST"; wanted = Gate::kBadHandshake; status = 400; break;
    case 8: version = 10; wanted = Gate::kBadHandshake; status = 400; break;
    case 9: input.push_back({"Sec-WebSocket-Protocol", "chat, chat"}); wanted = Gate::kBadHandshake; status = 400; break;
    case 10: input[2].second = "Upgrade,, keep-alive"; wanted = Gate::kBadHandshake; status = 400; break;
    default:
        input[1].second = "h2c/1.0, WebSocket"; input[2].second = " keep-alive, UpGrAdE\t";
        input[3].second = " \t" + key + "\t"; input[4].second = "\t13 ";
        input.push_back({"Sec-WebSocket-Protocol", "chat, other"}); break;
    }
    const Headers before = input;
    const auto result = host::check_websocket_upgrade(views(input), method, version);
    check(result == wanted && input == before);
    check(host::websocket_upgrade_rejection(result).status == status);
    necessary(input, result, method, version);

    // Raw field bytes reach both the handshake gate and the actual HTTP
    // normalization boundary; invalid bytes are never assumed sanitized.
    input = canonical(key);
    const char* names[] = {"Upgrade", "Connection", "Sec-WebSocket-Key", "Sec-WebSocket-Version",
                          "Sec-WebSocket-Protocol", "Host", "X-Fuzz"};
    const std::string bytes = size ? std::string(reinterpret_cast<const char*>(data), size) : "";
    input.push_back({names[size ? data[0] % 7 : 6], bytes});
    const auto raw = host::check_websocket_upgrade(views(input), "GET", 11);
    necessary(input, raw, "GET", 11);
    host::RequestRoutingPolicy policy;
    policy.public_scheme = "https"; policy.public_authority = "fuzz.test";
    const auto normalized = host::normalize_public_request(policy, "/@capsid/fuzz/ws", views(input));
    if (normalized.ok) {
        check(normalized.request.application == "fuzz");
        check(normalized.request.url == "https://fuzz.test/ws");
        for (const auto& h : normalized.request.headers) {
            check(h.name == lower(h.name) && h.name != "host" && h.name != "connection" && h.name != "upgrade");
            for (const unsigned char c : h.value) check(c == 9 || (c >= 32 && c <= 126));
        }
    } else {
        check(normalized.request.application.empty() && normalized.request.url.empty() && normalized.request.headers.empty());
    }
}

void response(const uint8_t* data, size_t size, const std::string& key) {
    std::string safe;
    for (size_t i = 0; i < std::min<size_t>(size, 64); ++i) safe += static_cast<char>(32 + data[i] % 95);
    Headers headers{{"X-Fuzz", safe}, {"Connection", "application-connection"},
        {"UPGRADE", "application-upgrade"}, {"Sec-WebSocket-Accept", "application-accept"},
        {"Sec-WebSocket-Extensions", "permessage-deflate"}, {"Content-Length", "1"},
        {"Transfer-Encoding", "chunked"}, {"X-End", "retained"}};
    std::string offered = "chat, other", selected;
    bool wanted = true;
    switch (size ? data[0] % 6 : 0) {
    case 0: break;
    case 1: headers.push_back({"Sec-WebSocket-Protocol", "chat"}); selected = "chat"; break;
    case 2: headers.push_back({"Sec-WebSocket-Protocol", "Chat"}); wanted = false; break;
    case 3: headers.push_back({"Sec-WebSocket-Protocol", "chat"}); headers.push_back({"Sec-WebSocket-Protocol", "chat"}); wanted = false; break;
    case 4: headers.push_back({"Sec-WebSocket-Protocol", "chat, other"}); wanted = false; break;
    default: offered = " \tchat,\tother "; headers.push_back({"SEC-WEBSOCKET-PROTOCOL", "other"}); selected = "other"; break;
    }
    std::string out = "sentinel";
    const bool ok = host::websocket_handshake(key, offered, headers, &out);
    check(ok == wanted);
    if (!ok) check(out == "sentinel");
    else {
        std::string expected = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept(key) + "\r\n";
        expected += "X-Fuzz: " + safe + "\r\nX-End: retained\r\n";
        if (!selected.empty()) expected += (selected == "other" ? "SEC-WEBSOCKET-PROTOCOL: " : "Sec-WebSocket-Protocol: ") + selected + "\r\n";
        check(out == expected + "\r\n");
    }
    std::string bad = key; bad[0] = '!'; out = "unchanged";
    check(!host::websocket_handshake(bad, offered, headers, &out) && out == "unchanged");
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::array<uint8_t, 16> nonce{};
    for (size_t i = 0; i < nonce.size(); ++i) nonce[i] = size ? data[i % size] : static_cast<uint8_t>(i);
    const std::string key = base64(nonce.data(), nonce.size());
    check(ws::validate_client_key(key));
    check(ws::compute_accept_key(key) == accept(key));
    check(ws::compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    const std::string raw = size ? std::string(reinterpret_cast<const char*>(data), size) : "";
    check(ws::compute_accept_key(raw) == accept(raw));
    if (ws::validate_client_key(raw)) {
        check(raw.size() == 24);
        std::array<unsigned char, 32> decoded{};
        check(EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(raw.data()), 24) == 18);
        check(raw.substr(22) == "==");
    }
    request(data, size, key);
    response(data, size, key);
    http(data, size, key);
    return 0;
}
