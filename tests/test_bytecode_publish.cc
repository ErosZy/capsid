#include "bytecode_publish.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Operations {
    int renamed, linked, removed;
    std::vector<std::string> calls;
    capsid::tools::PublishResult run() {
        return capsid::tools::publish_no_replace(
            [&]() { calls.emplace_back("rename"); return renamed; },
            [&]() { calls.emplace_back("link"); return linked; },
            [&]() { calls.emplace_back("unlink"); return removed; });
    }
};
void direct_success() {
    Operations operations{0, EIO, EIO, {}};
    const auto result = operations.run();
    require(result.complete && result.created_final && result.error == 0, "direct publication lost ownership");
    require(operations.calls == std::vector<std::string>{"rename"}, "unneeded fallback after successful rename");
}
void unsupported_fallback() {
    std::vector<int> errors{EINVAL, ENOSYS};
#ifdef EOPNOTSUPP
    errors.push_back(EOPNOTSUPP);
#endif
#ifdef ENOTSUP
    errors.push_back(ENOTSUP);
#endif
    for (const int error : errors) {
        Operations operations{error, 0, 0, {}};
        const auto result = operations.run();
        require(result.complete && result.created_final && result.error == 0, "supported fallback failed");
        require(operations.calls == std::vector<std::string>{"rename", "link", "unlink"}, "fallback order");
    }
}
void existing_final_is_never_retried() {
    Operations operations{EEXIST, 0, 0, {}};
    const auto result = operations.run();
    require(!result.complete && !result.created_final && result.error == EEXIST, "existing target was claimed");
    require(operations.calls.size() == 1, "existing target caused another publish attempt");
}
void ordinary_errors_are_not_fallback_candidates() {
    for (const int error : {EACCES, EPERM, ENOENT, ENOTDIR, EXDEV, ENOSPC, EIO, EROFS}) {
        Operations operations{error, 0, 0, {}};
        const auto result = operations.run();
        require(!result.complete && !result.created_final && result.error == error, "ordinary failure hidden");
        require(operations.calls.size() == 1, "ordinary error retried using fallback");
    }
}
void concurrent_creator_during_fallback_stays_foreign() {
    Operations operations{EINVAL, EEXIST, 0, {}};
    const auto result = operations.run();
    require(!result.complete && !result.created_final && result.error == EEXIST, "fallback claimed concurrent creator's file");
    require(operations.calls == std::vector<std::string>{"rename", "link"}, "foreign file caused temporary unlink");
}
void failed_link_keeps_its_error() {
    for (const int error : {EACCES, EPERM, EMLINK, ENOSPC, EXDEV, EIO}) {
        Operations operations{EINVAL, error, 0, {}};
        const auto result = operations.run();
        require(!result.complete && !result.created_final && result.error == error, "link failure hidden");
        require(std::string(result.operation) == "linkat" && operations.calls.size() == 2, "wrong link failure stage");
    }
}
void failed_temporary_unlink_keeps_final_ownership_for_rollback() {
    Operations operations{ENOSYS, 0, EACCES, {}};
    const auto result = operations.run();
    require(!result.complete && result.created_final && result.error == EACCES, "created final escaped rollback tracking");
    require(std::string(result.operation) == "unlink temporary", "wrong cleanup failure stage");
}
}  // namespace
int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"direct_success", direct_success}, {"unsupported_fallback", unsupported_fallback},
        {"existing_final_is_never_retried", existing_final_is_never_retried},
        {"ordinary_errors_are_not_fallback_candidates", ordinary_errors_are_not_fallback_candidates},
        {"concurrent_creator_during_fallback_stays_foreign", concurrent_creator_during_fallback_stays_foreign},
        {"failed_link_keeps_its_error", failed_link_keeps_its_error},
        {"failed_temporary_unlink_keeps_final_ownership_for_rollback", failed_temporary_unlink_keeps_final_ownership_for_rollback}};
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << "7 publication operation groups passed\n";
}
