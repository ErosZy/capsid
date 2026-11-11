#include "cold_start_measurement.h"
#include <functional>
#include <iostream>

namespace {
namespace cold = capsid::bench::cold;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function>
void rejects(Function function, const char* code = "") {
    try { function(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(code) != std::string::npos, "wrong failure reason");
        return;
    }
    throw std::runtime_error("invalid measurement accepted");
}
const cold::Identity root{10, 100, 1, 'S'};
struct FakeProc {
    cold::Processes processes{{10, root}, {20, {20, 101, 10, 'S'}}, {30, {30, 102, 20, 'S'}}, {40, {40, 99, 1, 'S'}}};
    std::map<int64_t, cold::Memory> memories{{10, {10, 20}}, {20, {20, 40}}, {30, {30, 60}}, {40, {9999, 9999}}};
    std::function<void(int64_t)> on_memory;
    std::function<void()> on_snapshot;
    cold::Processes snapshot() { if (on_snapshot) on_snapshot(); return processes; }
    cold::Identity identity(int64_t pid) {
        if (!processes.count(pid)) cold::fail("process-exited", "injected missing process");
        return processes.at(pid);
    }
    cold::Memory memory(int64_t pid) { if (on_memory) on_memory(pid); return memories.at(pid); }
};
void stat_identity() {
    std::vector<std::string> fields(20, "0");
    fields[0] = "S"; fields[1] = "3"; fields[19] = "12345";
    std::string text = "17 (command ) with (parentheses))";
    for (const auto& field : fields) text += " " + field;
    const auto parsed = cold::parse_stat(17, text);
    require(parsed.pid == 17 && parsed.parent == 3 && parsed.start == 12345 && parsed.state == 'S', "stat comm parsing");
    std::string zero = "17 (foreign process) S 0";
    for (int field = 2; field < 20; ++field) zero += " 0";
    require(cold::parse_stat(17, zero).start == 0, "zero is a numeric start tick, not a missing field");
    rejects([&]() { cold::parse_stat(18, text); }, "process-stat");
    rejects([]() { cold::parse_stat(17, "17 (truncated) S 3"); }, "process-stat");
}
void memory_format() {
    const auto valid = cold::parse_smaps("Rss: 40 kB\nPss_Anon: 3 kB\nPss: 20 kB\n");
    require(valid.pss == 20 && valid.rss == 40, "PSS parse");
    for (const auto& bad : {"", "Pss: 0 kB", "Pss: -1 kB", "Pss: 1.5 kB", "Pss: 3 B", "Pss: 2 kB extra",
                            "Pss: 1 kB\nPss: 1 kB", "Pss: 18446744073709551616 kB"}) {
        rejects([&]() { cold::parse_smaps(std::string(bad) + "\nRss: 40 kB\n"); });
    }
    rejects([]() { cold::parse_smaps("Pss: 1 kB\nRss: 0 kB\n"); });
}
void argument_ranges() {
    for (const auto& bad : {"", "0", "-1", "+1", "1x", " 1", "10001", "18446744073709551616"}) {
        rejects([&]() { cold::bounded_count(bad, 1, 10000, "iterations"); });
    }
    require(cold::bounded_count("0", 0, 50, "warm") == 0, "explicit zero warm count");
    require(cold::bounded_count("50", 0, 50, "warm") == 50, "count boundary");
}
void response_bytes() {
    for (size_t split = 0; split <= cold::kExpectedBody.size(); ++split) {
        cold::ResponseCheck response(7);
        response.head(7, 200, false);
        response.body(7, cold::kExpectedBody.substr(0, split));
        response.body(7, cold::kExpectedBody.substr(split));
        response.end(7);
        rejects([&]() { response.body(7, ""); }, "response-order");
    }
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 500, false); }, "response-head");
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 200, true); }, "response-head");
    rejects([]() { cold::ResponseCheck r(7); r.body(7, "x"); }, "response-body");
    rejects([]() { cold::ResponseCheck r(7); r.end(7); }, "response-end");
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 200, false); r.head(7, 200, false); }, "response-head");
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 200, false); r.body(8, cold::kExpectedBody); }, "response-order");
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 200, false); r.body(7, cold::kExpectedBody.substr(0, 10)); r.end(7); }, "response-end");
    rejects([]() { cold::ResponseCheck r(7); r.head(7, 200, false); r.body(7, std::string(cold::kExpectedBody) + "x"); }, "response-body");
}
void recursive_ownership() {
    FakeProc reader;
    const auto pinned = cold::tree(root, reader.snapshot());
    require(pinned.size() == 3 && !pinned.count(40), "recursive descendants and foreign sibling");
    auto sample = cold::sample_tree(reader, root, pinned);
    require(sample.pss == 60 && sample.rss == 120 && sample.parts.size() == 3, "recursive memory sum");
    reader.processes.at(30).parent = 1;
    sample = cold::sample_tree(reader, root, pinned);
    require(sample.pss == 60 && sample.parts.size() == 3, "pinned reparented grandchild");
}
void missing_or_replaced_member() {
    for (int scenario = 0; scenario < 5; ++scenario) {
        FakeProc reader;
        const auto pinned = cold::tree(root, reader.snapshot());
        if (scenario == 0) reader.processes.erase(30);  // Already reaped, root still alive.
        if (scenario == 1) reader.processes.at(30).state = 'Z';
        if (scenario == 2) ++reader.processes.at(30).start;
        if (scenario == 3) reader.processes.emplace(31, cold::Identity{31, 103, 20, 'S'});
        if (scenario == 4) reader.processes.erase(10);
        rejects([&]() { cold::sample_tree(reader, root, pinned); }, "process-");
    }
}
void memory_read_races() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        FakeProc reader;
        const auto pinned = cold::tree(root, reader.snapshot());
        reader.on_memory = [&](int64_t pid) {
            if (pid != 20) return;
            if (scenario == 0) ++reader.processes.at(20).start;
            if (scenario == 1) reader.processes.erase(30);
            if (scenario == 2) reader.processes.at(10).state = 'Z';
        };
        rejects([&]() { cold::sample_tree(reader, root, pinned); });
    }
}
void failed_and_overflowing_memory() {
    FakeProc reader;
    const auto pinned = cold::tree(root, reader.snapshot());
    reader.on_memory = [](int64_t) { cold::fail("unreadable", "injected permission error"); };
    rejects([&]() { cold::sample_tree(reader, root, pinned); }, "unreadable");
    reader.on_memory = {};
    reader.memories.at(20).pss = 0;
    rejects([&]() { cold::sample_tree(reader, root, pinned); }, "pss-range");
    reader.memories.at(20).pss = std::numeric_limits<uint64_t>::max();
    rejects([&]() { cold::sample_tree(reader, root, pinned); }, "pss-range");
}
cold::Sample sample(uint64_t pss) { return {pss, pss * 2, {{root, {pss, pss * 2}}}}; }
void bounded_stability() {
    const std::vector<uint64_t> values{10, 11, 12, 12};
    size_t index = 0;
    int sleeps = 0, observed = 0;
    const auto settled = cold::stable_samples([&]() { return sample(values.at(index++)); }, [&]() { ++sleeps; },
        [&](const auto&) { ++observed; }, 8);
    require(settled.size() == 4 && sleeps == 3 && observed == 4 && settled.back().pss == 12, "bounded stable retry");
    index = 0;
    rejects([&]() { cold::stable_samples([&]() { return sample(values.at(index++)); }, []() {}, [](const auto&) {}, 3); }, "pss-unstable");
    require(index == 3, "drift exceeded attempt bound");
    rejects([]() { cold::stable_samples([]() -> cold::Sample { cold::fail("read-failed", "unreadable"); }, []() {}, [](const auto&) {}, 8); }, "read-failed");
    rejects([]() { cold::stable_samples([]() { return sample(0); }, []() {}, [](const auto&) {}, 8); }, "pss-nonpositive");
}
void stable_total_does_not_hide_replacement() {
    int reads = 0;
    rejects([&]() { cold::stable_samples([&]() {
        auto value = sample(10);
        value.parts[0].identity.start += static_cast<uint64_t>(reads++);
        return value;
    }, []() {}, [](const auto&) {}, 8); }, "process-changed");
}
}  // namespace
int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"stat_identity", stat_identity}, {"memory_format", memory_format}, {"argument_ranges", argument_ranges},
        {"response_bytes", response_bytes}, {"recursive_ownership", recursive_ownership},
        {"missing_or_replaced_member", missing_or_replaced_member}, {"memory_read_races", memory_read_races},
        {"failed_and_overflowing_memory", failed_and_overflowing_memory}, {"bounded_stability", bounded_stability},
        {"stable_total_does_not_hide_replacement", stable_total_does_not_hide_replacement}};
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << "10 cold measurement model groups passed\n";
}
