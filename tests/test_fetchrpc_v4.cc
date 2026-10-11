// FetchRPC v4 conformance harness (TDD boundary stage).
//
// Contract: docs/fetchrpc-v4.md. Golden vectors:
// tests/fixtures/fetchrpc-v4/vectors.json — frozen with the contract.
//
// Today (v3 shipped) this harness enforces the parts of the contract
// that must already hold:
//   * the vector data is well-formed (hex decodes, expected outcomes
//     are from the contract vocabulary);
//   * every v4-versioned vector is rejected deterministically by the
//     shipped v3 parser with the exact error string — the
//     "v3 and v4 peers must never be mixed silently" boundary.
// When the v4 surface lands, the same vectors grow full round-trip
// and sequence assertions; the boundary checks stay forever.
#include "protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' ||
                     s[a] == '\t')) {
        ++a;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\n' ||
                     s[b - 1] == '\r' || s[b - 1] == '\t')) {
        --b;
    }
    return s.substr(a, b - a);
}

// Minimal JSON string-field reader: returns the raw value text of the
// first "key":"value" occurrence. The vector file is generated data
// with a fixed shape; a real parser is not warranted for the boundary
// stage (and json-c is a host dependency, not a worker-protocol one).
std::string find_string_field(const std::string &text,
                              const std::string &key) {
    const std::string needle = "\"" + key + "\":";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return "";
    }
    pos = text.find('"', pos + needle.size());
    if (pos == std::string::npos) {
        return "";
    }
    const size_t start = pos + 1;
    const size_t end = text.find('"', start);
    if (end == std::string::npos) {
        return "";
    }
    return text.substr(start, end - start);
}

std::vector<std::string> find_all_string_fields(const std::string &text,
                                                const std::string &key) {
    std::vector<std::string> out;
    const std::string needle = "\"" + key + "\":";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        size_t q = text.find('"', pos + needle.size());
        if (q == std::string::npos) {
            break;
        }
        const size_t start = q + 1;
        const size_t end = text.find('"', start);
        if (end == std::string::npos) {
            break;
        }
        out.push_back(text.substr(start, end - start));
        pos = end;
    }
    return out;
}

std::vector<uint8_t> unhex(const std::string &hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = std::stoi(hex.substr(i, 1), nullptr, 16);
        const int lo = std::stoi(hex.substr(i + 1, 1), nullptr, 16);
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : nullptr;
    if (!path) {
        std::fprintf(stderr, "usage: %s <vectors.json>\n", argv[0]);
        return 2;
    }
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    const std::vector<std::string> names =
        find_all_string_fields(text, "name");
    const std::vector<std::string> hexes =
        find_all_string_fields(text, "hex");
    const std::vector<std::string> expects =
        find_all_string_fields(text, "expect");

    require(!hexes.empty(), "vector file carries frames");
    require(hexes.size() == expects.size(),
            "every frame has hex and expect");
    require(names.size() >= hexes.size(),
            "every frame is named (sequences carry names too)");

    for (size_t i = 0; i < hexes.size(); ++i) {
        const std::vector<uint8_t> wire = unhex(hexes[i]);
        require(!wire.empty(), "hex decodes: " + names[i]);
        require(wire.size() > capsid::protocol::kHeaderSize,
                "wire carries a header: " + names[i]);

        // The frozen boundary: the shipped v3 parser must reject every
        // v4-versioned vector, with the exact contract error string.
        capsid::protocol::Parser parser;
        capsid::protocol::Frame frame;
        require(parser.append(wire.data(), wire.size()),
                "parser buffers bytes: " + names[i]);
        require(parser.next(&frame) == capsid::protocol::kParseError,
                "v3 parser rejects v4 frame: " + names[i]);
        require(parser.error() == "unsupported protocol version",
                "v3 rejection names the version: " + names[i] +
                    " (got: " + parser.error() + ")");
        require(expects[i].rfind("v3-parser-reject", 0) == 0,
                "expect starts with the v3 boundary: " + names[i]);
    }

    const std::vector<std::string> seq_steps =
        find_all_string_fields(text, "steps");
    require(seq_steps.size() >= 6,
            "contract sequences are present in the data");

#ifdef FETCHRPC_V4_SURFACE
    // v4 round-trip: every vector decodes under a v4 parser with the
    // expected type and re-encodes byte-identically.
    for (size_t i = 0; i < hexes.size(); ++i) {
        // expected-error vectors have dedicated assertions below
        if (expects[i].find("v4-parser-error") != std::string::npos) {
            continue;
        }
        const std::vector<uint8_t> wire = unhex(hexes[i]);
        capsid::protocol::Parser v4(4);
        capsid::protocol::Frame frame;
        require(v4.append(wire.data(), wire.size()), "v4 buffers: " + names[i]);
        require(v4.next(&frame) == capsid::protocol::kParseFrame,
                "v4 parses: " + names[i]);
        std::vector<uint8_t> re;
        require(capsid::protocol::encode_versioned(4, frame, &re),
                "v4 encodes: " + names[i]);
        require(re == wire, "v4 round trip is byte-identical: " + names[i]);
    }

    // negotiation: a v4 parser rejects a v3-versioned frame.
    {
        capsid::protocol::Frame f;
        f.type = capsid::protocol::kHello;
        f.flags = 0;
        f.request_id = 0;
        std::vector<uint8_t> wire;
        require(capsid::protocol::encode(f, &wire), "v3 hello encodes");
        capsid::protocol::Parser v4(4);
        capsid::protocol::Frame out;
        require(v4.append(wire.data(), wire.size()), "v4 buffers v3 bytes");
        require(v4.next(&out) == capsid::protocol::kParseError,
                "v4 rejects v3 frame");
        require(v4.error() == "unsupported protocol version",
                "v4 rejection names the version (got: " + v4.error() + ")");
    }

    // hello v4 capability validation: unknown bit and nonzero reserved.
    for (const char *case_name : {"hello-v4-unknown-cap-bit",
                                  "hello-v4-reserved-nonzero"}) {
        const std::vector<std::string>::const_iterator it =
            std::find(names.begin(), names.end(), case_name);
        require(it != names.end(), std::string("vector present: ") + case_name);
        const std::vector<uint8_t> wire = unhex(hexes[it - names.begin()]);
        capsid::protocol::Parser v4(4);
        capsid::protocol::Frame frame;
        require(v4.append(wire.data(), wire.size()), "v4 buffers");
        require(v4.next(&frame) == capsid::protocol::kParseError,
                std::string("v4 rejects bad hello: ") + case_name);
        require(v4.error() == "invalid hello capabilities",
                std::string("hello rejection names capabilities (got: ") +
                    v4.error() + ")");
    }

    // v3 parser must reject v4-only types even at version 3.
    {
        capsid::protocol::Frame f;
        f.type = capsid::protocol::kChannelClose;
        f.flags = 0;
        f.request_id = 0;
        std::vector<uint8_t> wire;
        require(!capsid::protocol::encode(f, &wire),
                "v3 encode rejects kChannelClose");
    }
#endif

    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("fetchrpc-v4 boundary vectors: PASS\n");
    return 0;
}
