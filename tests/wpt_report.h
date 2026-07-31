#ifndef CAPSID_TESTS_WPT_REPORT_H
#define CAPSID_TESTS_WPT_REPORT_H

#include <string>

namespace capsid_test {

// Parses the top-level JSON object and proves that a WPT realm actually ran
// at least one subtest. The parser is deliberately outside the JavaScript
// fixture so an empty realm cannot make both producer and consumer agree on a
// false success.
bool validate_wpt_nonzero_report(const std::string &json,
                                 std::string *error);

}  // namespace capsid_test

#endif
