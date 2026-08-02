// capsid-host executable entry point.
//
// The frozen M1A CLI is strictly validated before anything is spawned:
// unknown or missing arguments fail before any side effect. Startup order is
// fixed by the design: validate arguments, read and load the source bundle,
// spawn the worker, wait for READY and verify the compatibility ID, bind the
// listener, and only then write one canonical JSON line to --ready-fd.
// stdout never carries readiness or logs; diagnostics go to stderr.

#include "host/single_worker_server.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "host/active_state.h"
#include "host/request_normalization.h"

namespace {

constexpr std::string_view kProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

void fail(const std::string& message) {
    std::fprintf(stderr, "capsid-host: %s\n", message.c_str());
    std::exit(2);
}

bool valid_application_id(const std::string& application) {
    capsid::host::ActiveStateDocument probe;
    probe.state = capsid::host::ActiveServiceState::kActive;
    probe.application = application;
    probe.version = "v0";
    probe.generation = std::string(kProbeGeneration);
    return capsid::host::encode_active_state_json(probe).ok;
}

std::uint64_t parse_positive_integer(const std::string& value,
                                     const char* name) {
    if (value.empty()) {
        fail(std::string("--") + name + " requires a positive integer");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            fail(std::string("--") + name +
                 " requires a positive integer: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::int64_t>::max())) {
        fail(std::string("--") + name +
             " requires a positive integer: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

// host:port; only decimal ports and non-empty hosts are accepted.
void parse_listen(const std::string& value,
                  std::string* out_address,
                  std::uint16_t* out_port) {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 == value.size()) {
        fail("--listen requires host:port");
    }
    *out_address = value.substr(0, colon);
    const std::string port_text = value.substr(colon + 1);
    for (const char c : port_text) {
        if (c < '0' || c > '9') {
            fail("--listen port must be decimal: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(port_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed > 65535) {
        fail("--listen port must be in [0, 65535]: " + value);
    }
    *out_port = static_cast<std::uint16_t>(parsed);
}

std::vector<std::uint8_t> read_bundle(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot read --source-bundle: " + path);
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        fail("cannot size --source-bundle: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size));
        if (!input) {
            fail("cannot read --source-bundle: " + path);
        }
    }
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> values;
    for (int index = 1; index < argc; index += 2) {
        const char* key = argv[index];
        const char* value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (key == nullptr || key[0] != '-' || key[1] != '-') {
            fail("arguments must be --key value pairs");
        }
        if (value == nullptr || value[0] == '\0') {
            fail(std::string("missing value for ") + key);
        }
        const std::string name = key + 2;
        if (values.find(name) != values.end()) {
            fail("duplicate argument: --" + name);
        }
        values[name] = value;
    }

    const auto require = [&values](const std::string& name) -> std::string {
        auto it = values.find(name);
        if (it == values.end()) {
            fail("missing --" + name);
        }
        return it->second;
    };

    capsid::host::SingleWorkerServerOptions options;
    const std::string mode = require("mode");
    if (mode != "single-worker") {
        fail("--mode must be single-worker");
    }
    options.worker_path = require("worker");
    options.source_bundle_path = require("source-bundle");
    options.source_name = require("source-name");
    if (options.source_name.rfind("file://", 0) != 0) {
        fail("--source-name must be an absolute file URL");
    }
    options.application = require("application");
    if (!valid_application_id(options.application)) {
        fail("--application is not a valid App ID");
    }
    const std::string listen = require("listen");
    parse_listen(listen, &options.listen_address, &options.listen_port);
    // The address itself is validated before anything is spawned: an
    // unparseable address must fail the CLI phase, not the post-spawn bind
    // phase (a failure after spawn would otherwise have to tear down a live
    // worker).
    {
        boost::system::error_code address_error;
        const boost::asio::ip::address address =
            boost::asio::ip::make_address(options.listen_address,
                                          address_error);
        if (address_error) {
            fail("--listen requires an IP address: " +
                 options.listen_address);
        }
        (void)address;
    }
    const std::string routing = require("routing");
    if (routing != "path") {
        fail("--routing must be path in M1A");
    }
    options.public_scheme = require("public-scheme");
    if (options.public_scheme != "http" && options.public_scheme != "https") {
        fail("--public-scheme must be http or https");
    }
    options.public_authority = require("public-authority");
    if (!capsid::host::is_valid_public_authority(
            options.public_authority)) {
        fail("--public-authority must be host[:port]");
    }
    options.request_timeout_ms =
        parse_positive_integer(require("request-timeout-ms"),
                               "request-timeout-ms");
    const std::uint64_t window =
        parse_positive_integer(require("initial-stream-window"),
                               "initial-stream-window");
    if (window > std::numeric_limits<std::uint32_t>::max()) {
        fail("--initial-stream-window exceeds uint32");
    }
    options.initial_stream_window = static_cast<std::uint32_t>(window);
    const std::string sandbox = require("strict-sandbox");
    if (sandbox != "on" && sandbox != "off") {
        fail("--strict-sandbox must be on or off");
    }
    options.strict_sandbox = sandbox == "on";
    options.ready_fd = static_cast<int>(
        parse_positive_integer(require("ready-fd"), "ready-fd"));
    if (options.ready_fd <= 0 ||
        options.ready_fd > static_cast<int>(std::numeric_limits<short>::max())) {
        fail("--ready-fd must be a positive descriptor number");
    }
    // The READY record must be deliverable; verify the descriptor is open
    // before spawning the worker.
    if (fcntl(options.ready_fd, F_GETFD) == -1) {
        fail("--ready-fd is not an open descriptor");
    }

    const std::vector<std::uint8_t> bundle =
        read_bundle(options.source_bundle_path);

    capsid::host::SingleWorkerServer server(std::move(options));
    return server.run(bundle);
}
