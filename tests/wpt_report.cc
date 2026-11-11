#include "wpt_report.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <map>

namespace capsid_test {
namespace {

class JsonCursor {
public:
    explicit JsonCursor(const std::string &text) : text_(text), offset_(0) {}

    bool top_level_scalars(std::map<std::string, std::string> *fields,
                           std::string *error) {
        skip_space();
        if (!consume('{')) {
            return fail(error, "WPT report must be a JSON object");
        }
        skip_space();
        if (consume('}')) {
            skip_space();
            return at_end(error);
        }
        for (;;) {
            std::string key;
            if (!parse_string(&key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                return fail(error, "missing ':' after JSON object key");
            }
            skip_space();
            const size_t value_begin = offset_;
            if (!skip_value(0, error)) {
                return false;
            }
            if (fields->count(key) != 0) {
                return fail(error, "duplicate WPT report field: " + key);
            }
            (*fields)[key] = text_.substr(value_begin, offset_ - value_begin);
            skip_space();
            if (consume('}')) {
                skip_space();
                return at_end(error);
            }
            if (!consume(',')) {
                return fail(error, "missing ',' between JSON object fields");
            }
            skip_space();
        }
    }

private:
    bool skip_value(unsigned depth, std::string *error) {
        if (depth > 64) {
            return fail(error, "JSON nesting is too deep");
        }
        skip_space();
        if (offset_ >= text_.size()) {
            return fail(error, "missing JSON value");
        }
        if (text_[offset_] == '"') {
            std::string ignored;
            return parse_string(&ignored, error);
        }
        if (consume('{')) {
            skip_space();
            if (consume('}')) {
                return true;
            }
            for (;;) {
                std::string ignored;
                if (!parse_string(&ignored, error)) {
                    return false;
                }
                skip_space();
                if (!consume(':')) {
                    return fail(error, "missing ':' in nested JSON object");
                }
                if (!skip_value(depth + 1, error)) {
                    return false;
                }
                skip_space();
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return fail(error, "missing ',' in nested JSON object");
                }
                skip_space();
            }
        }
        if (consume('[')) {
            skip_space();
            if (consume(']')) {
                return true;
            }
            for (;;) {
                if (!skip_value(depth + 1, error)) {
                    return false;
                }
                skip_space();
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return fail(error, "missing ',' in JSON array");
                }
                skip_space();
            }
        }

        const size_t begin = offset_;
        while (offset_ < text_.size()) {
            const char value = text_[offset_];
            if (std::isspace(static_cast<unsigned char>(value)) ||
                value == ',' || value == '}' || value == ']') {
                break;
            }
            ++offset_;
        }
        if (offset_ == begin) {
            return fail(error, "empty JSON scalar");
        }
        const std::string scalar = text_.substr(begin, offset_ - begin);
        if (scalar == "true" || scalar == "false" || scalar == "null") {
            return true;
        }
        size_t digit = scalar[0] == '-' ? 1 : 0;
        if (digit == scalar.size()) {
            return fail(error, "invalid JSON scalar");
        }
        if (scalar[digit] == '0') {
            ++digit;
        } else if (scalar[digit] >= '1' && scalar[digit] <= '9') {
            do { ++digit; } while (digit < scalar.size() &&
                scalar[digit] >= '0' && scalar[digit] <= '9');
        } else {
            return fail(error, "invalid JSON scalar");
        }
        if (digit < scalar.size() && scalar[digit] == '.') {
            const size_t fraction = ++digit;
            while (digit < scalar.size() && scalar[digit] >= '0' && scalar[digit] <= '9') {
                ++digit;
            }
            if (digit == fraction) {
                return fail(error, "invalid JSON fraction");
            }
        }
        if (digit < scalar.size() && (scalar[digit] == 'e' || scalar[digit] == 'E')) {
            ++digit;
            if (digit < scalar.size() && (scalar[digit] == '+' || scalar[digit] == '-')) {
                ++digit;
            }
            const size_t exponent = digit;
            while (digit < scalar.size() && scalar[digit] >= '0' && scalar[digit] <= '9') {
                ++digit;
            }
            if (digit == exponent) {
                return fail(error, "invalid JSON exponent");
            }
        }
        return digit == scalar.size() || fail(error, "invalid JSON scalar");
    }

    bool parse_string(std::string *output, std::string *error) {
        if (!consume('"')) {
            return fail(error, "expected JSON string");
        }
        output->clear();
        while (offset_ < text_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(text_[offset_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20) {
                return fail(error, "control character in JSON string");
            }
            if (value != '\\') {
                output->push_back(static_cast<char>(value));
                continue;
            }
            if (offset_ >= text_.size()) {
                return fail(error, "truncated JSON escape");
            }
            const char escaped = text_[offset_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output->push_back(escaped);
                    break;
                case 'b':
                    output->push_back('\b');
                    break;
                case 'f':
                    output->push_back('\f');
                    break;
                case 'n':
                    output->push_back('\n');
                    break;
                case 'r':
                    output->push_back('\r');
                    break;
                case 't':
                    output->push_back('\t');
                    break;
                case 'u': {
                    if (offset_ + 4 > text_.size()) {
                        return fail(error, "truncated JSON unicode escape");
                    }
                    for (unsigned index = 0; index < 4; ++index) {
                        if (!std::isxdigit(static_cast<unsigned char>(
                                text_[offset_ + index]))) {
                            return fail(error, "invalid JSON unicode escape");
                        }
                    }
                    const unsigned code = static_cast<unsigned>(std::strtoul(
                        text_.substr(offset_, 4).c_str(), NULL, 16));
                    // Decode escaped ASCII keys as well, so e.g. passed and
                    // \u0070assed cannot evade duplicate-field detection.
                    if (code < 0x80) {
                        output->push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        output->push_back(static_cast<char>(0xc0 | (code >> 6)));
                        output->push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        output->push_back(static_cast<char>(0xe0 | (code >> 12)));
                        output->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        output->push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    offset_ += 4;
                    break;
                }
                default:
                    return fail(error, "invalid JSON escape");
            }
        }
        return fail(error, "unterminated JSON string");
    }

    void skip_space() {
        while (offset_ < text_.size() &&
               (text_[offset_] == ' ' || text_[offset_] == '\t' ||
                text_[offset_] == '\n' || text_[offset_] == '\r')) {
            ++offset_;
        }
    }

    bool consume(char value) {
        if (offset_ < text_.size() && text_[offset_] == value) {
            ++offset_;
            return true;
        }
        return false;
    }

    bool at_end(std::string *error) const {
        if (offset_ != text_.size()) {
            return fail(error, "trailing data after WPT JSON report");
        }
        return true;
    }

    static bool fail(std::string *error, const std::string &message) {
        if (error) {
            *error = message;
        }
        return false;
    }

    const std::string &text_;
    size_t offset_;
};

bool parse_positive_integer(const std::string &value, unsigned long long *out) {
    if (value.empty()) {
        return false;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    if (value.size() > 1 && value[0] == '0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

}  // namespace

bool validate_wpt_nonzero_report(const std::string &json,
                                 std::string *error) {
    std::map<std::string, std::string> fields;
    JsonCursor cursor(json);
    if (!cursor.top_level_scalars(&fields, error)) {
        return false;
    }
    if (fields.count("total") == 0) {
        if (error) {
            *error = "WPT report is missing integer field 'total'";
        }
        return false;
    }
    unsigned long long total = 0;
    if (!parse_positive_integer(fields["total"], &total)) {
        if (error) {
            *error = "WPT report field 'total' is not an unsigned integer";
        }
        return false;
    }
    if (total == 0) {
        if (error) {
            *error = "WPT realm executed zero subtests";
        }
        return false;
    }
    if (fields.count("ranNothing") == 0) {
        if (error) {
            *error = "WPT report is missing boolean field 'ranNothing'";
        }
        return false;
    }
    if (fields["ranNothing"] != "false") {
        if (error) {
            *error = fields["ranNothing"] == "true"
                         ? "WPT realm reported ranNothing=true"
                         : "WPT report field 'ranNothing' is not a boolean";
        }
        return false;
    }
    return true;
}

bool validate_wpt_success_report(const std::string &json, std::string *error) {
    if (!validate_wpt_nonzero_report(json, error)) {
        return false;
    }
    std::map<std::string, std::string> fields;
    JsonCursor cursor(json);
    if (!cursor.top_level_scalars(&fields, error)) {
        return false;
    }
    const auto require_value = [&](const char *field, const char *expected) {
        if (fields.count(field) && fields[field] == expected) {
            return true;
        }
        if (error) {
            *error = std::string("unexpected or missing top-level WPT field: ") + field;
        }
        return false;
    };
    if (!require_value("profile", "\"CAPSID-MIN-2025-subset-v0\"") ||
        !require_value("source", "\"web-platform-tests/wpt\"") ||
        !require_value("passed", "true")) {
        return false;
    }
    for (const char *field : {"failures", "missingExpectedFailures"}) {
        const std::string &value = fields[field];
        if (value.size() < 2 || value.front() != '[' || value.back() != ']' ||
            value.find_first_not_of(" \r\n\t", 1) != value.size() - 1) {
            if (error) {
                *error = std::string("WPT report requires an empty array: ") + field;
            }
            return false;
        }
    }
    return true;
}

}  // namespace capsid_test
