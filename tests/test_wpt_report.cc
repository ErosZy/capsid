#include "wpt_report.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void expect_valid(const std::string &json) {
    std::string error;
    require(capsid_test::validate_wpt_nonzero_report(json, &error),
            error.c_str());
}

void expect_invalid(const std::string &json) {
    std::string error;
    require(!capsid_test::validate_wpt_nonzero_report(json, &error),
            "invalid WPT report was accepted");
    require(!error.empty(), "invalid WPT report had no diagnostic");
}

}  // namespace

int main() {
    expect_valid("{\"total\":1,\"ranNothing\":false}");
    expect_valid(
        "{ \"meta\":{\"total\":0}, \"ranNothing\" : false, "
        "\"message\":\"\\\"total\\\":0\", \"total\" : 17 }");

    expect_invalid("{\"total\":0,\"ranNothing\":false}");
    expect_invalid("{\"total\" : 0,\"ranNothing\":false}");
    expect_invalid("{\"total\":0.0,\"ranNothing\":false}");
    expect_invalid("{\"total\":-1,\"ranNothing\":false}");
    expect_invalid("{\"total\":\"1\",\"ranNothing\":false}");
    expect_invalid("{\"ranNothing\":false}");
    expect_invalid("{\"total\":1}");
    expect_invalid("{\"total\":1,\"ranNothing\":true}");
    expect_invalid("{\"total\":1,\"ranNothing\":\"false\"}");
    expect_invalid(
        "{\"total\":1,\"total\":2,\"ranNothing\":false}");
    expect_invalid(
        "{\"total\":1,\"ranNothing\":false} trailing");

    // A nested success must not conceal a failing top-level report, and
    // contradictory or syntactically invalid reports are never evidence.
    const std::string good =
        "\"profile\":\"CAPSID-MIN-2025-subset-v0\","
        "\"source\":\"web-platform-tests/wpt\",\"total\":1,"
        "\"ranNothing\":false,\"passed\":true,\"failures\":[],"
        "\"missingExpectedFailures\":[]";
    const auto check_success = [](const std::string &json, bool valid) {
        std::string error;
        const bool result = capsid_test::validate_wpt_success_report(json, &error);
        require(result == valid, ("unexpected WPT success verdict: " + json).c_str());
        require(valid || !error.empty(), "rejected report needs a diagnostic");
    };
    check_success("{" + good + "}", true);
    check_success("{" + good + ",\"meta\":{\"passed\":false}}", true);
    check_success("{" + good + ",\"passed\":false}", false);
    check_success("{" + good + ",\"\\u0070assed\":false}", false);
    check_success("{" + good + ",\"meta\":undefined}", false);
    check_success("{" + good + ",\"meta\":01}", false);
    check_success("{" + good + ",\"meta\":1e}", false);
    check_success("{" + good + ",\"meta\":-1.25e+2}", true);
    check_success("{\"total\":1,\"ranNothing\":false,"
                  "\"nested\":{\"passed\":true,\"failures\":[]}}", false);
    for (const std::string field : {"profile", "source", "passed", "failures", "missingExpectedFailures"}) {
        std::string changed = good;
        const size_t start = changed.find("\"" + field + "\":");
        const size_t value = start + field.size() + 3;
        const size_t end = changed.find(',', value);
        changed.replace(value, (end == std::string::npos ? changed.size() : end) - value,
                        field == "passed" ? "false" : "[{}]");
        check_success("{" + changed + "}", false);
    }
    return 0;
}
