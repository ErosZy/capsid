#ifndef CAPSID_HOST_SECRET_FILE_PROVIDER_H
#define CAPSID_HOST_SECRET_FILE_PROVIDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// Secret file provider (M1D). Secrets are read beneath a pre-opened App
// secret directory descriptor (secretRootTemplate/<app>), which the caller
// must have opened and verified (owner/mode) beforehand. key IDs are single
// filename components — never paths.
//
// Contract:
//   - O_NONBLOCK + O_NOFOLLOW open, regular file only (a FIFO can never
//     block the Host);
//   - at most kMaxSecretFileBytes + 1 bytes; NUL bytes and invalid UTF-8
//     reject;
//   - the revision is the frozen "file-v1:<dev>:<inode>:<size>:
//     <ctime-sec>:<ctime-nsec>" string;
//   - identity (dev/inode/size/mtime/ctime) is re-checked after the read;
//     any change fails the file;
//   - values enter only the owning snapshot — never error strings, logs or
//     serialized output.

inline constexpr std::size_t kMaxSecretFileBytes = 16U * 1024U + 1U;
inline constexpr std::size_t kMaxSecretKeyIdBytes = 128U;
inline constexpr std::size_t kMaxSecretsPerSnapshot = 256U;

struct SecretFileOutcome {
    // Static error text; never contains the value or its digest.
    std::string error;
    // Frozen revision string (empty on failure).
    std::string revision;
    // The secret value. It may only be copied into the owning Runtime
    // environment snapshot.
    std::vector<std::uint8_t> value;
};

// Reads exactly the requested key_ids beneath secret_dir_fd and returns one
// outcome per request in the same order. Duplicate request entries are
// rejected (the caller must deduplicate); a missing or invalid file fails
// that entry only. key_id must be a single valid filename component.
std::vector<SecretFileOutcome> read_secret_files(
    int secret_dir_fd,
    const std::vector<std::string>& key_ids);

// A key_id is a valid single filename component: non-empty, no '/', no NUL,
// not "." or "..", and within kMaxSecretKeyIdBytes.
bool valid_secret_key_id(const std::string& key_id);

}  // namespace capsid::host

#endif
