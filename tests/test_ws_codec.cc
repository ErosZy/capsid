// RFC 6455 codec tests (FetchRPC v4 / WS-C3, TDD).
//
// Contract: docs/fetchrpc-v4.md (D1 — the host owns framing) and
// docs/workerd-websocket-semantics.md §5. The codec is self-contained:
// SHA-1 + base64 for the accept key, incremental frame parser, and a
// role-aware encoder (clients mask, servers do not).
#include "ws_codec.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::vector<uint8_t> bytes(const std::string &hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(
            std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

bool hex_eq(const std::vector<uint8_t> &a, const std::string &hex) {
    return a == bytes(hex);
}

capsid::host::ws::Frame text_frame(const std::string &s) {
    capsid::host::ws::Frame f;
    f.fin = true;
    f.opcode = capsid::host::ws::kText;
    f.payload.assign(s.begin(), s.end());
    return f;
}

}  // namespace

int main() {
    using namespace capsid::host::ws;

    // ---- accept key: the RFC 6455 §1.3 worked example ----
    require(compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
                "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
            "RFC 6455 accept-key example");
    require(compute_accept_key("x3JJHMbTw1kut5mHtJEnuA==") ==
                "piLLsJNUU2uhcIeDL6bycTh7UEM=",
            "second accept-key vector (python-verified)");

    // ---- client key validation ----
    require(validate_client_key("dGhlIHNhbXBsZSBub25jZQ=="),
            "16-byte base64 nonce validates");
    require(!validate_client_key("short"), "short key rejected");
    require(!validate_client_key("!!!!!!!!!!!!!!!!!!!!!!=="),
            "non-base64 key rejected");

    // ---- server encode: RFC 6455 §5.7 unmasked Hello ----
    {
        std::vector<uint8_t> out;
        require(encode(text_frame("Hello"), kServer, 0, &out),
                "server text encodes");
        require(hex_eq(out, "810548656c6c6f"),
                "server frame matches RFC example");
        Parser client_sees(kClient);
        Frame seen;
        require(client_sees.append(out.data(), out.size()), "client buffers");
        require(client_sees.next(&seen) == kParseFrame,
                "server frame parses at the client");
    }

    // ---- client encode: masked, deterministic mask, round trip ----
    {
        std::vector<uint8_t> out;
        require(encode(text_frame("Hello"), kClient, 0x01020304u, &out),
                "client text encodes");
        require((out[1] & 0x80) != 0, "client frame is masked");
        Parser parser(kServer);
        Frame decoded;
        require(parser.append(out.data(), out.size()), "parser buffers");
        require(parser.next(&decoded) == kParseFrame, "masked frame parses");
        require(decoded.opcode == kText && decoded.fin,
                "masked round trip keeps opcode/fin");
        require(std::string(decoded.payload.begin(), decoded.payload.end()) ==
                    "Hello",
                "masked round trip unmaskes payload");
    }

    // ---- length forms ----
    {
        for (size_t len : {125u, 126u, 65535u, 65536u, 1000000u}) {
            Frame f;
            f.fin = true;
            f.opcode = kBinary;
            f.payload.assign(len, 0x5a);
            std::vector<uint8_t> out;
            require(encode(f, kServer, 0, &out), "length form encodes");
            Parser parser(kClient);
            Frame decoded;
            require(parser.append(out.data(), out.size()), "buffers");
            require(parser.next(&decoded) == kParseFrame, "parses");
            require(decoded.payload.size() == len,
                    "payload length round trips: " + std::to_string(len));
        }
    }

    // ---- control frames ----
    {
        Frame ping;
        ping.fin = true;
        ping.opcode = kPing;
        ping.payload.assign(4, 0x11);
        Frame close;
        close.fin = true;
        close.opcode = kClose;
        const uint16_t code = 3000;
        close.payload.push_back(static_cast<uint8_t>(code >> 8));
        close.payload.push_back(static_cast<uint8_t>(code));
        const char *reason = "planned";
        close.payload.insert(close.payload.end(), reason, reason + 7);
        for (const Frame &f : {ping, close}) {
            std::vector<uint8_t> out;
            require(encode(f, kServer, 0, &out), "control encodes");
            Parser parser(kClient);
            Frame decoded;
            require(parser.append(out.data(), out.size()), "buffers");
            require(parser.next(&decoded) == kParseFrame, "control parses");
            require(decoded.opcode == f.opcode, "control opcode round trips");
        }
    }

    // ---- fragmentation (FIN=0 + continuation) ----
    {
        Frame first;
        first.fin = false;
        first.opcode = kText;
        first.payload.assign(3, 'a');
        Frame cont;
        cont.fin = true;
        cont.opcode = kContinuation;
        cont.payload.assign(2, 'b');
        std::vector<uint8_t> part1;
        std::vector<uint8_t> part2;
        require(encode(first, kServer, 0, &part1), "fragment encodes");
        require(encode(cont, kServer, 0, &part2), "continuation encodes");
        std::vector<uint8_t> wire = part1;
        wire.insert(wire.end(), part2.begin(), part2.end());
        Parser parser(kClient);
        Frame decoded;
        require(parser.append(wire.data(), wire.size()), "buffers");
        require(parser.next(&decoded) == kParseFrame, "first fragment");
        require(!decoded.fin && decoded.opcode == kText, "first fragment shape");
        require(parser.next(&decoded) == kParseFrame, "second fragment");
        require(decoded.fin && decoded.opcode == kContinuation,
                "continuation shape");
    }

    // ---- incremental byte-at-a-time ----
    {
        std::vector<uint8_t> out;
        require(encode(text_frame("stream"), kServer, 0, &out), "encodes");
        Parser parser(kClient);
        Frame decoded;
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            require(parser.append(&out[i], 1), "byte accepted");
            require(parser.next(&decoded) == kParseNeedMore,
                    "partial frame waits");
        }
        require(parser.append(&out[out.size() - 1], 1), "last byte");
        require(parser.next(&decoded) == kParseFrame, "complete frame");
    }

    // ---- violations ----
    {
        // RSV bits set
        Parser p(kServer);
        Frame f;
        require(p.append(bytes("c10548656c6c6f").data(), 7), "rsv buffered");
        require(p.next(&f) == kParseError && p.error() == "reserved bits set",
                "RSV bits rejected: " + p.error());

        // reserved opcodes 3..7 and 11..15
        Parser p2(kServer);
        require(p2.append(bytes("8300").data(), 2), "opcode buffered");
        require(p2.next(&f) == kParseError && p2.error() == "reserved opcode",
                "opcode 3 rejected: " + p2.error());

        // control frame over 125 bytes
        Frame big_ping;
        big_ping.fin = true;
        big_ping.opcode = kPing;
        big_ping.payload.assign(126, 0);
        std::vector<uint8_t> out;
        require(!encode(big_ping, kServer, 0, &out),
                "oversized control frame does not encode");

        // fragmented control frame
        Frame frag_ping;
        frag_ping.fin = false;
        frag_ping.opcode = kPing;
        std::vector<uint8_t> out2;
        require(!encode(frag_ping, kServer, 0, &out2),
                "fragmented control frame does not encode");

        // unmasked client frame seen by the server
        Parser p3(kServer);
        require(p3.append(bytes("810548656c6c6f").data(), 7), "buffered");
        require(p3.next(&f) == kParseError &&
                    p3.error() == "client frame not masked",
                "unmasked client frame rejected: " + p3.error());

        // masked server frame seen by the client
        Parser p4(kClient);
        std::vector<uint8_t> masked;
        require(encode(text_frame("x"), kClient, 1, &masked), "encodes");
        require(p4.append(masked.data(), masked.size()), "buffered");
        require(p4.next(&f) == kParseError &&
                    p4.error() == "server frame must not be masked",
                "masked server frame rejected: " + p4.error());
    }

    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ws codec: PASS\n");
    return 0;
}
