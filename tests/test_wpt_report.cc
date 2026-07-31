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
    return 0;
}
