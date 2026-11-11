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

int failures_probe = 0;

void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures_probe;
    std::exit(1);
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

    // ---- T03: consumed-prefix compaction and bounded memory ----
    {
        // Interleave complete frames with a held partial frame: the
        // consumed prefix must be compacted instead of accumulating.
        Parser parser(kClient);
        Frame decoded;
        std::vector<uint8_t> small;
        require(encode(text_frame("x"), kServer, 0, &small), "encodes");
        require(small.size() == 3, "fixture frame shape");
        // Each iteration delivers one frame in two chunks (partial
        // header state, then completion): the consumed prefix must be
        // compacted, so storage stays bounded across thousands of
        // interleaved partial/complete deliveries.
        size_t peak = 0;
        for (int i = 0; i < 2000; ++i) {
            require(parser.append(small.data(), 2),
                    "partial frame accepted");
            require(parser.storage_bytes() > 0, "partial buffered");
            require(parser.append(small.data() + 2, 1),
                    "frame completion accepted");
            require(parser.next(&decoded) == kParseFrame,
                    "completed frame parses");
            if (parser.storage_bytes() > peak) {
                peak = parser.storage_bytes();
            }
            if (peak > 8192) {
                fail("parser storage grows without compaction: " +
                     std::to_string(peak));
            }
        }
        require(peak <= small.size() * 2 + 64,
                "storage stayed bounded at: " + std::to_string(peak));
    }

    // ---- T03: over-budget input fails BEFORE allocation ----
    {
        Parser parser(kClient);
        // Cap is 64 MiB; a single huge append must fail without the
        // buffer having grown (check via storage accounting after the
        // rejected append).
        std::vector<uint8_t> huge(70u * 1024u * 1024u, 0x81);
        require(!parser.append(huge.data(), huge.size()),
                "oversized append rejected");
        require(parser.storage_bytes() == 0,
                "rejected append must not grow storage");
    }

    // ---- T03: all-zero mask with MASK bit set is legal ----
    {
        std::vector<uint8_t> out;
        require(encode(text_frame("z"), kClient, 0, &out),
                "client frame with all-zero mask encodes");
        require((out[1] & 0x80) != 0, "MASK bit is set");
        require(out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0,
                "zero mask key serialized");
        Parser parser(kServer);
        Frame decoded;
        require(parser.append(out.data(), out.size()), "buffers");
        require(parser.next(&decoded) == kParseFrame, "parses");
        require(std::string(decoded.payload.begin(),
                            decoded.payload.end()) == "z",
                "zero-mask payload round trips");
        // Server frames still must not be masked.
        Frame server_frame = text_frame("s");
        std::vector<uint8_t> server_out;
        require(encode(server_frame, kServer, 0, &server_out),
                "server unmasked encodes");
        require((server_out[1] & 0x80) == 0, "server MASK bit clear");
    }

    // ---- T03: minimal length encodings (RFC 6455 §5.2) ----
    {
        // length==126 form must carry >= 126; ==127 form >= 65536;
        // a 64-bit length with the MSB set is rejected outright.
        Parser parser(kClient);
        Frame f;
        // 126-length form carrying 100 (< 126): non-minimal.
        std::vector<uint8_t> bad_short;
        bad_short.push_back(0x82);
        bad_short.push_back(126);
        bad_short.push_back(0);
        bad_short.push_back(100);
        bad_short.insert(bad_short.end(), 100, 0x7a);
        require(parser.append(bad_short.data(), bad_short.size()),
                "buffers");
        require(parser.next(&f) == kParseError &&
                    parser.error() == "non-minimal length encoding",
                "126-form under 126 rejected: " + parser.error());

        Parser parser2(kClient);
        std::vector<uint8_t> bad_medium;
        bad_medium.push_back(0x82);
        bad_medium.push_back(127);
        for (int i = 0; i < 8; ++i) {
            bad_medium.push_back(i == 5 ? 1 : 0);  // = 65536+... no: build
        }
        // Simpler: 127-form carrying 1000 (< 65536).
        std::vector<uint8_t> bad_medium2;
        bad_medium2.push_back(0x82);
        bad_medium2.push_back(127);
        uint64_t small_len = 1000;
        for (int i = 7; i >= 0; --i) {
            bad_medium2.push_back(
                static_cast<uint8_t>(small_len >> (i * 8)));
        }
        bad_medium2.insert(bad_medium2.end(), 1000, 0x7a);
        require(parser2.append(bad_medium2.data(), bad_medium2.size()),
                "buffers");
        require(parser2.next(&f) == kParseError &&
                    parser2.error() == "non-minimal length encoding",
                "127-form under 65536 rejected: " + parser2.error());

        Parser parser3(kClient);
        std::vector<uint8_t> bad_msb;
        bad_msb.push_back(0x82);
        bad_msb.push_back(127);
        bad_msb.push_back(0x80);  // MSB set: reserved/absurd
        for (int i = 0; i < 7; ++i) {
            bad_msb.push_back(0);
        }
        require(parser3.append(bad_msb.data(), bad_msb.size()),
                "buffers");
        require(parser3.next(&f) == kParseError,
                "64-bit MSB length rejected");
    }

    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ws codec: PASS\n");
    return 0;
}
