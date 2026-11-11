#ifndef CAPSID_BENCH_COLD_START_MEASUREMENT_H
#define CAPSID_BENCH_COLD_START_MEASUREMENT_H

// Measurement-only rules. Tests inject process tables, reads and clock waits.
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace capsid::bench::cold {
inline constexpr std::string_view kExpectedBody = R"({"status":"ok","total":2039,"count":16})";

[[noreturn]] inline void fail(const char* code, const std::string& message) {
    throw std::runtime_error(std::string(code) + ": " + message);
}

inline uint64_t unsigned_decimal(std::string_view value, const char* field) {
    if (value.empty()) fail("number-format", std::string("invalid ") + field);
    uint64_t result = 0;
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) fail("number-format", std::string("invalid ") + field);
    return result;
}

inline int bounded_count(std::string_view value, int minimum, int maximum, const char* field) {
    const uint64_t count = unsigned_decimal(value, field);
    if (count < static_cast<uint64_t>(minimum) || count > static_cast<uint64_t>(maximum)) {
        fail("argument-range", std::string(field) + " is outside its allowed range");
    }
    return static_cast<int>(count);
}

struct Identity {
    int64_t pid = 0;
    uint64_t start = 0;
    int64_t parent = 0;
    char state = '?';
    bool same(const Identity& other) const { return pid == other.pid && start == other.start; }
    bool alive() const { return state != 'Z' && state != 'X' && state != 'x'; }
};
using Processes = std::map<int64_t, Identity>;

inline Identity parse_stat(int64_t pid, const std::string& text) {
    const auto open = text.find('('), close = text.rfind(')');
    const std::string prefix = std::to_string(pid) + " ";
    if (pid <= 0 || open != prefix.size() || text.compare(0, open, prefix) != 0 ||
        close == std::string::npos || close <= open || close + 2 >= text.size()) fail("process-stat", "invalid process identity");
    std::istringstream in(text.substr(close + 2));
    std::vector<std::string> fields;
    for (std::string field; in >> field;) fields.push_back(field);
    if (fields.size() < 20 || fields[0].size() != 1) fail("process-stat", "incomplete process identity");
    const uint64_t parent = unsigned_decimal(fields[1], "parent PID");
    const uint64_t start = unsigned_decimal(fields[19], "process start time");
    if (parent > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) fail("process-stat", "out-of-range process identity");
    return {pid, start, static_cast<int64_t>(parent), fields[0][0]};
}

struct Memory { uint64_t pss = 0, rss = 0; };
inline Memory parse_smaps(const std::string& text) {
    Memory value;
    bool seen_pss = false, seen_rss = false;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const bool pss = line.rfind("Pss:", 0) == 0, rss = line.rfind("Rss:", 0) == 0;
        if (!pss && !rss) continue;
        std::istringstream row(line);
        std::string field, number, unit, extra;
        if (!(row >> field >> number >> unit) || unit != "kB" || row >> extra || (pss ? seen_pss : seen_rss)) {
            fail("pss-format", "malformed or duplicate memory field");
        }
        const uint64_t kb = unsigned_decimal(number, "memory kB");
        if (kb == 0) fail("pss-nonpositive", "nonpositive process memory");
        if (pss) { value.pss = kb; seen_pss = true; }
        else { value.rss = kb; seen_rss = true; }
    }
    if (!seen_pss || !seen_rss) fail("pss-format", "missing PSS or RSS");
    return value;
}

inline Processes tree(const Identity& root, const Processes& snapshot, const Processes& pinned = {}) {
    const auto found = snapshot.find(root.pid);
    if (found == snapshot.end() || !root.same(found->second) || !found->second.alive()) {
        fail("process-exited", "measured worker exited or changed identity");
    }
    Processes owned{{root.pid, found->second}};
    for (const auto& [pid, identity] : pinned) {
        const auto current = snapshot.find(pid);
        if (current == snapshot.end() || !identity.same(current->second) || !current->second.alive()) {
            fail("process-changed", "a pinned worker-tree member exited or was replaced");
        }
        owned[pid] = current->second;  // Retain ownership after reparenting.
    }
    for (;;) {
        const size_t before = owned.size();
        for (const auto& [pid, identity] : snapshot) {
            if (owned.count(pid) || !owned.count(identity.parent)) continue;
            if (!identity.alive() || identity.start < owned.at(identity.parent).start) fail("process-changed", "invalid or exited worker descendant");
            owned.emplace(pid, identity);
        }
        if (owned.size() == before) break;
    }
    if (!pinned.empty() && owned.size() != pinned.size()) fail("process-changed", "worker-tree membership grew after it was pinned");
    return owned;
}

struct Part { Identity identity; Memory memory; };
struct Sample { uint64_t pss = 0, rss = 0; std::vector<Part> parts; };
inline void checked_add(uint64_t& total, uint64_t part) {
    if (part == 0 || part > std::numeric_limits<uint64_t>::max() - total) fail("pss-range", "nonpositive or overflowing memory sum");
    total += part;
}

template<class Reader>
Sample sample_tree(Reader& reader, const Identity& root, const Processes& pinned) {
    const Processes before = tree(root, reader.snapshot(), pinned);
    Sample sample;
    for (const auto& [pid, identity] : before) {
        const Identity first = reader.identity(pid);
        if (!identity.same(first) || !first.alive()) fail("process-changed", "process changed before memory read");
        const Memory memory = reader.memory(pid);
        const Identity after = reader.identity(pid);
        if (!identity.same(after) || !after.alive()) fail("process-changed", "process changed during memory read");
        checked_add(sample.pss, memory.pss);
        checked_add(sample.rss, memory.rss);
        sample.parts.push_back({identity, memory});
    }
    tree(root, reader.snapshot(), before);
    return sample;
}

template<class Sampler, class Sleep, class Observe>
std::vector<Sample> stable_samples(Sampler sampler, Sleep sleep, Observe observe, int attempts) {
    if (attempts < 2 || attempts > 64) fail("pss-attempts", "expected 2..64 bounded PSS attempts");
    std::vector<Sample> samples;
    for (int i = 0; i < attempts; ++i) {
        if (i) sleep();
        Sample sample = sampler();
        if (sample.pss == 0 || sample.parts.empty()) fail("pss-nonpositive", "invalid memory sample");
        observe(sample);
        const bool equal = !samples.empty() && samples.back().pss == sample.pss;
        if (equal) {
            if (samples.back().parts.size() != sample.parts.size()) fail("process-changed", "PSS membership changed");
            for (size_t p = 0; p < sample.parts.size(); ++p) {
                if (!sample.parts[p].identity.same(samples.back().parts[p].identity)) fail("process-changed", "PSS membership changed");
            }
        }
        samples.push_back(std::move(sample));
        if (equal) return samples;
    }
    fail("pss-unstable", "positive PSS did not settle to two equal readings within the attempt bound");
}

class ResponseCheck {
public:
    explicit ResponseCheck(uint64_t request) : request_(request) {}
    void head(uint64_t request, uint32_t status, bool upgraded) {
        check_id(request);
        if (head_ || status != 200 || upgraded) fail("response-head", "expected one HTTP 200 response head");
        head_ = true;
    }
    void body(uint64_t request, std::string_view bytes) {
        check_id(request);
        if (!head_ || bytes.size() > kExpectedBody.size() - body_.size() || kExpectedBody.substr(body_.size(), bytes.size()) != bytes) {
            fail("response-body", "response bytes differ from the frozen fixture");
        }
        body_.append(bytes);
    }
    void end(uint64_t request) {
        check_id(request);
        if (!head_ || body_ != kExpectedBody) fail("response-end", "missing head or incomplete response body");
        ended_ = true;
    }
private:
    void check_id(uint64_t request) const {
        if (request != request_ || ended_) fail("response-order", "unexpected request identity or terminal event");
    }
    uint64_t request_;
    bool head_ = false, ended_ = false;
    std::string body_;
};
}  // namespace capsid::bench::cold
#endif
