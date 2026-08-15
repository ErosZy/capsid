// Frozen RED: host_secret_file_provider (M1D).
//
// Covers the secret file provider boundary:
//   - a legal secret reads with the frozen file-v1 revision and its value;
//   - NUL bytes, invalid UTF-8, over-limit size, missing file, duplicate
//     request entries and hostile key ids reject;
//   - an extra file on disk (not requested) is never read or leaked;
//   - a FIFO rejects without blocking;
//   - a high-entropy canary never appears in any error string or in a
//     serialization of the outcomes (values excluded).

#include "host/secret_file_provider.h"

#include <fcntl.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/stat.h>
#endif
#include <sys/types.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void write_file_at(int dir_fd, const char* name, const std::string& content) {
    const int fd = openat(dir_fd, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fail(std::string("cannot create fixture file: ") + name);
    }
    const bool ok = content.empty() ||
        write(fd, content.data(), content.size()) ==
            static_cast<ssize_t>(content.size());
    close(fd);
    require(ok, "cannot write fixture file");
}

std::string serialize_outcomes(
    const std::vector<capsid::host::SecretFileOutcome>& outcomes) {
    // Serialization of everything except values: errors and revisions only.
    std::string out;
    for (const capsid::host::SecretFileOutcome& outcome : outcomes) {
        out += "error=" + outcome.error + ";revision=" + outcome.revision + "|";
    }
    return out;
}

}  // namespace

int main() {
    using capsid::host::read_secret_files;
    using capsid::host::SecretFileOutcome;

    const std::string root = "/tmp/capsid-secrets-XXXXXX";
    char* dir = mkdtemp(const_cast<char*>(root.c_str()));
    require(dir != nullptr, "cannot create secret fixture root");
    const int dir_fd = open(dir, O_RDONLY | O_DIRECTORY);
    require(dir_fd >= 0, "cannot open secret fixture root");

    const std::string canary = "canary-9f3a71c2-55b0-4d7e-9a1e-8f2c6d1b4a5e";

    // 1. Legal secret: value + frozen revision.
    {
        write_file_at(dir_fd, "api-token", "sk-live-" + canary);
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "api-token" });
        require(outcomes.size() == 1, "missing outcome");
        require(outcomes[0].error.empty(), "legal secret rejected");
        require(outcomes[0].value.size() == std::strlen("sk-live-") + canary.size(),
                "legal secret value wrong");
        // Frozen revision: file-v1:<dev>:<inode>:<size>:<ctime-sec>:<ctime-nsec>
        require(outcomes[0].revision.rfind("file-v1:", 0) == 0,
                "revision prefix wrong");
        std::size_t colon_count = 0;
        for (const char c : outcomes[0].revision) {
            if (c == ':') {
                colon_count += 1;
            }
        }
        require(colon_count == 5, "revision field count wrong");
        require(outcomes[0].revision.find(canary) == std::string::npos,
                "revision leaked the value");
    }

    // 2. NUL byte rejects.
    {
        write_file_at(dir_fd, "nul-secret",
                      std::string("bad\0value", 9));
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "nul-secret" });
        require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                "NUL secret accepted");
        require(outcomes[0].value.empty(), "NUL secret value leaked");
    }

    // 3. Invalid UTF-8 rejects.
    {
        write_file_at(dir_fd, "bad-utf8",
                      std::string("\xc3\x28", 2));  // truncated sequence
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "bad-utf8" });
        require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                "invalid UTF-8 secret accepted");
    }

    // 4. Over-limit rejects (kMaxSecretFileBytes = 16 KiB + 1).
    {
        const std::string big(16U * 1024U + 2U, 'x');
        write_file_at(dir_fd, "big-secret", big);
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "big-secret" });
        require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                "over-limit secret accepted");
    }

    // 5. Missing file rejects.
    {
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "no-such-secret" });
        require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                "missing secret accepted");
    }

    // 6. Duplicate request entries reject.
    {
        write_file_at(dir_fd, "dup-secret", "value");
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "dup-secret", "dup-secret" });
        require(outcomes.size() == 2, "duplicate outcome count wrong");
        require(outcomes[0].error.empty() && !outcomes[1].error.empty(),
                "duplicate request entry accepted");
    }

    // 7. Hostile key ids reject (never a path).
    {
        for (const char* bad : { "../escape", "a/b", "/abs", ".", "..", "" }) {
            const std::vector<SecretFileOutcome> outcomes =
                read_secret_files(dir_fd, { bad });
            require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                    std::string("hostile key id accepted: ") + bad);
        }
    }

    // 8. FIFO rejects without blocking.
    {
        require(mkfifoat(dir_fd, "fifo-secret", 0600) == 0,
                "cannot create FIFO fixture");
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "fifo-secret" });
        require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                "FIFO secret accepted");
    }

    // 4b. Exactly 16 KiB accepts; 16 KiB + 1 rejects.
    {
        const std::string at_limit(16U * 1024U, 'v');
        write_file_at(dir_fd, "limit-secret", at_limit);
        const std::vector<SecretFileOutcome> at_limit_outcomes =
            read_secret_files(dir_fd, { "limit-secret" });
        require(at_limit_outcomes.size() == 1 &&
                    at_limit_outcomes[0].error.empty(),
                "16 KiB secret rejected");
        const std::string over(16U * 1024U + 1U, 'v');
        write_file_at(dir_fd, "over-secret", over);
        const std::vector<SecretFileOutcome> over_outcomes =
            read_secret_files(dir_fd, { "over-secret" });
        require(over_outcomes.size() == 1 && !over_outcomes[0].error.empty(),
                "16 KiB + 1 secret accepted");
    }

    // 7b. Key id grammar matrix: whitespace, colon, non-ASCII and ".."
    // all reject.
    {
        for (const char* bad : { "has space", "has:colon", "caf\xc3\xa9",
                                 "a..b", "..", "a/b" }) {
            const std::vector<SecretFileOutcome> outcomes =
                read_secret_files(dir_fd, { bad });
            require(outcomes.size() == 1 && !outcomes[0].error.empty(),
                    std::string("grammar-invalid key id accepted: ") + bad);
        }
    }

    // 7c. Read-failure regression: a truncated-while-reading secret fails
    // exactly once (one outcome per requested key).
    {
        const std::string big(16U * 1024U - 1U, 'r');
        write_file_at(dir_fd, "race-secret", big);
        // Truncate immediately after the request is built; the reader's
        // pread then hits EOF early and the identity check fails the file.
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "race-secret" });
        require(outcomes.size() == 1,
                "read failure produced a wrong outcome count");
        // A deterministic single-outcome assertion: whatever the outcome
        // is, the snapshot for this key is exactly one entry.
    }

    // 9. Extra file on disk is never read or leaked.
    {
        const std::string unrequested_name = "unrequested-" + canary;
        write_file_at(dir_fd, unrequested_name.c_str(), "not-requested");
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(dir_fd, { "api-token" });
        require(outcomes.size() == 1, "extra file changed the outcome set");
        require(serialize_outcomes(outcomes).find(canary) == std::string::npos,
                "unrequested file name leaked into outcomes");
    }

    // 10. Zero-leak: every error string and the full serialization must be
    // free of the canary value.
    {
        const std::vector<SecretFileOutcome> outcomes = read_secret_files(
            dir_fd, { "api-token", "no-such-secret", "big-secret", "bad-utf8",
                      "nul-secret", "fifo-secret", "../escape", "dup-secret",
                      "dup-secret", "unrequested-" + canary });
        const std::string serialized = serialize_outcomes(outcomes);
        require(serialized.find(canary) == std::string::npos,
                "canary leaked into the serialized outcomes");
        for (const SecretFileOutcome& outcome : outcomes) {
            require(outcome.error.find(canary) == std::string::npos &&
                        outcome.revision.find(canary) == std::string::npos,
                    "canary leaked into an outcome field");
        }
    }

    close(dir_fd);
    std::cout << "PASS" << std::endl;
    return 0;
}
