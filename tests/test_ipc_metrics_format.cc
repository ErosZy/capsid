#include "host/ipc_metrics.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}
using Values = std::map<std::string, std::uint64_t>;

Values object(std::string_view text, std::size_t& offset) {
    Values values;
    require(offset < text.size() && text[offset++] == '{', "object start");
    for (;;) {
        require(offset < text.size() && text[offset++] == '"', "field quote");
        const auto end = text.find('"', offset);
        require(end != std::string_view::npos, "field end");
        const std::string key(text.substr(offset, end - offset));
        offset = end + 1;
        require(offset < text.size() && text[offset++] == ':', "field separator");
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data() + offset, text.data() + text.size(), value);
        require(parsed.ec == std::errc{}, "exact unsigned integer: " + key);
        offset = static_cast<std::size_t>(parsed.ptr - text.data());
        require(values.emplace(key, value).second, "duplicate field: " + key);
        require(offset < text.size(), "object end");
        if (text[offset++] == '}') return values;
        require(text[offset - 1] == ',', "field delimiter");
    }
}
std::pair<Values, Values> parse(const std::string& text) {
    const std::string_view prefix = "{\"host\":";
    require(text.starts_with(prefix), "host object is present");
    std::size_t offset = prefix.size();
    Values host = object(text, offset);
    const std::string_view middle = ",\"client\":";
    require(std::string_view(text).substr(offset).starts_with(middle), "client object is present");
    offset += middle.size();
    Values client = object(text, offset);
    require(std::string_view(text).substr(offset) == "}\n", "exactly one complete JSON line");
    return {std::move(host), std::move(client)};
}
void compare(const Values& actual, const Values& expected, const char* group) {
    require(actual.size() == expected.size(), std::string(group) + " field count");
    for (const auto& [key, value] : expected) {
        const auto found = actual.find(key);
        require(found != actual.end(), std::string(group) + " missing " + key);
        require(found->second == value, std::string(group) + '.' + key +
            " expected=" + std::to_string(value) + " actual=" + std::to_string(found->second));
    }
}
void run_case(std::uint64_t seed, bool distinct) {
    using H = capsid::host::HostIpcMetrics;
    using C = capsid::ClientIpcMetrics;
    H host;
    C client;
    const std::array<std::pair<const char*, std::uint64_t H::*>, 15> host_fields = {{
        {"commands_submitted", &H::commands_submitted}, {"command_batches", &H::command_batches},
        {"commands_executed", &H::commands_executed}, {"flush_calls", &H::flush_calls},
        {"flush_eagain", &H::flush_eagain}, {"events_queued", &H::events_queued},
        {"asio_posts", &H::asio_posts}, {"response_heads", &H::response_heads},
        {"response_body_frames", &H::response_body_frames}, {"response_ends", &H::response_ends},
        {"grant_commands", &H::grant_commands}, {"credit_bytes_granted", &H::credit_bytes_granted},
        {"credit_stall_count", &H::credit_stall_count}, {"command_queue_hw", &H::command_queue_high_water},
        {"event_queue_hw", &H::event_queue_high_water},
    }};
    const std::array<std::pair<const char*, std::atomic<std::uint64_t> C::*>, 13> client_fields = {{
        {"queued_frames", &C::queued_frames}, {"queued_wire_bytes", &C::queued_wire_bytes},
        {"queue_would_block", &C::queue_would_block}, {"flush_calls", &C::flush_calls},
        {"socket_write_calls", &C::socket_write_calls}, {"socket_write_bytes", &C::socket_write_bytes},
        {"socket_write_eagain", &C::socket_write_eagain}, {"next_event_calls", &C::next_event_calls},
        {"parsed_frames", &C::parsed_frames}, {"parser_payload_copied_bytes", &C::parser_payload_copied_bytes},
        {"socket_read_calls", &C::socket_read_calls}, {"socket_read_bytes", &C::socket_read_bytes},
        {"socket_read_eagain", &C::socket_read_eagain},
    }};
    Values expected_host, expected_client;
    std::uint64_t index = 0;
    for (const auto& [name, member] : host_fields) {
        const auto value = seed + (distinct ? ++index : 0);
        host.*member = value;
        expected_host[name] = value;
    }
    for (const auto& [name, member] : client_fields) {
        const auto value = seed + (distinct ? ++index : 0);
        (client.*member).store(value);
        expected_client[name] = value;
    }
    const auto high_water = static_cast<std::size_t>(seed + (distinct ? ++index : 0));
    client.queued_bytes_high_water.store(high_water);
    expected_client["queued_bytes_hw"] = high_water;
    std::FILE* output = nullptr;
#if defined(_WIN32)
    require(tmpfile_s(&output) == 0, "temporary output");
#else
    output = std::tmpfile();
#endif
    require(output != nullptr, "temporary output exists");
    capsid::host::write_ipc_metrics_line(output, host, client);
    require(std::fflush(output) == 0 && std::fseek(output, 0, SEEK_SET) == 0, "read actual formatter output");
    char buffer[4096];
    const auto bytes = std::fread(buffer, 1, sizeof(buffer), output);
    require(std::feof(output) != 0 && std::ferror(output) == 0, "complete bounded output");
    require(std::fclose(output) == 0, "close output");
    const auto observed = parse(std::string(buffer, bytes));
    compare(observed.first, expected_host, "host");
    compare(observed.second, expected_client, "client");
    std::cout << "PASS: actual IPC formatter, seed=" << seed << " distinct=" << distinct << '\n';
}
}  // namespace

int main() {
    run_case(0, false);
    run_case((UINT64_C(1) << 32) + 7, true);
    run_case((UINT64_C(1) << 40) + 3, true);
    run_case(std::numeric_limits<std::uint64_t>::max(), false);
    return 0;
}
