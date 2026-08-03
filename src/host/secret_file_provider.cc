// Secret file provider (M1D). See secret_file_provider.h.

#include "host/secret_file_provider.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <set>
#include <sstream>

namespace capsid::host {
namespace {

constexpr const char* kSecretFileRevisionPrefix = "file-v1:";

bool valid_utf8(const std::uint8_t* data, std::size_t size) {
    std::size_t index = 0;
    while (index < size) {
        const std::uint8_t lead = data[index];
        if (lead <= 0x7f) {
            index += 1;
            continue;
        }
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if ((lead & 0xe0) == 0xc0) {
            continuation = 1;
            codepoint = lead & 0x1f;
            if (codepoint < 2) {
                return false;  // overlong
            }
        } else if ((lead & 0xf0) == 0xe0) {
            continuation = 2;
            codepoint = lead & 0x0f;
        } else if ((lead & 0xf8) == 0xf0) {
            continuation = 3;
            codepoint = lead & 0x07;
        } else {
            return false;
        }
        if (index + continuation >= size) {
            return false;
        }
        for (std::size_t step = 1; step <= continuation; ++step) {
            const std::uint8_t byte = data[index + step];
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;  // overlong, out of range, or surrogate
        }
        index += continuation + 1;
    }
    return true;
}

#if defined(__APPLE__)
#define CAPSID_SECRET_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_SECRET_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define CAPSID_SECRET_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_SECRET_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

std::string build_revision(const struct stat& st) {
    std::ostringstream out;
    out << kSecretFileRevisionPrefix << static_cast<std::uint64_t>(st.st_dev)
        << ':' << static_cast<std::uint64_t>(st.st_ino) << ':'
        << static_cast<std::uint64_t>(st.st_size) << ':'
        << static_cast<std::int64_t>(CAPSID_SECRET_CTIME_SEC(st)) << ':'
        << static_cast<std::int64_t>(CAPSID_SECRET_CTIME_NSEC(st));
    return out.str();
}

}  // namespace

bool valid_secret_key_id(const std::string& key_id) {
    if (key_id.empty() || key_id.size() > kMaxSecretKeyIdBytes ||
        key_id == "." || key_id == ".." ||
        key_id.find('/') != std::string::npos ||
        key_id.find('\0') != std::string::npos) {
        return false;
    }
    return true;
}

std::vector<SecretFileOutcome> read_secret_files(
    int secret_dir_fd,
    const std::vector<std::string>& key_ids) {
    std::vector<SecretFileOutcome> outcomes;
    outcomes.reserve(key_ids.size());
    if (secret_dir_fd < 0 || key_ids.size() > kMaxSecretsPerSnapshot) {
        SecretFileOutcome failure;
        failure.error = "invalid secret provider arguments";
        outcomes.push_back(failure);
        return outcomes;
    }

    std::set<std::string> seen;
    for (const std::string& key_id : key_ids) {
        SecretFileOutcome outcome;
        if (!valid_secret_key_id(key_id)) {
            outcome.error = "invalid secret key id";
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (!seen.insert(key_id).second) {
            outcome.error = "duplicate secret key id";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // O_NONBLOCK: opening a FIFO returns immediately instead of
        // blocking the Host; the regular-file check below rejects it.
        const int fd = openat(secret_dir_fd, key_id.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                outcome.error = "secret file does not exist";
            } else if (errno == ELOOP) {
                outcome.error = "secret path contains a symlink";
            } else if (errno == ENXIO) {
                outcome.error = "secret path is not a regular file";
            } else {
                outcome.error = "cannot open secret file";
            }
            outcomes.push_back(std::move(outcome));
            continue;
        }

        struct stat before = {};
        if (fstat(fd, &before) != 0) {
            close(fd);
            outcome.error = "cannot stat secret file";
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (!S_ISREG(before.st_mode)) {
            close(fd);
            outcome.error = "secret path is not a regular file";
            outcomes.push_back(std::move(outcome));
            continue;
        }
        const std::uint64_t size = static_cast<std::uint64_t>(before.st_size);
        if (size > kMaxSecretFileBytes) {
            close(fd);
            outcome.error = "secret file exceeds the size limit";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        outcome.value.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < outcome.value.size()) {
            const ssize_t count =
                pread(fd, outcome.value.data() + offset,
                      outcome.value.size() - offset, static_cast<off_t>(offset));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                outcome.value.clear();
                outcome.error = "secret file read failed";
                outcomes.push_back(std::move(outcome));
                offset = outcome.value.size() + 1;  // mark invalid
                break;
            }
            if (count == 0) {
                break;  // truncated; identity check below catches it
            }
            offset += static_cast<std::size_t>(count);
        }
        if (offset != outcome.value.size()) {
            close(fd);
            outcome.value.clear();
            outcome.error = "secret file changed while reading";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        struct stat after = {};
        if (fstat(fd, &after) != 0) {
            close(fd);
            outcome.value.clear();
            outcome.error = "cannot re-stat secret file";
            outcomes.push_back(std::move(outcome));
            continue;
        }
        if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size ||
            CAPSID_SECRET_CTIME_SEC(before) != CAPSID_SECRET_CTIME_SEC(after) ||
            CAPSID_SECRET_CTIME_NSEC(before) !=
                CAPSID_SECRET_CTIME_NSEC(after) ||
            CAPSID_SECRET_CTIME_SEC(before) !=
                CAPSID_SECRET_CTIME_SEC(after)) {
            close(fd);
            outcome.value.clear();
            outcome.error = "secret file changed while reading";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        // Value validation: no NUL bytes, valid UTF-8. On rejection the
        // value is cleared before it can leave this function.
        bool invalid = false;
        for (const std::uint8_t byte : outcome.value) {
            if (byte == 0) {
                invalid = true;
                break;
            }
        }
        if (!invalid &&
            !valid_utf8(outcome.value.data(), outcome.value.size())) {
            invalid = true;
        }
        if (invalid) {
            close(fd);
            outcome.value.clear();
            outcome.error = "secret file is not valid UTF-8";
            outcomes.push_back(std::move(outcome));
            continue;
        }

        outcome.revision = build_revision(after);
        close(fd);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

}  // namespace capsid::host
