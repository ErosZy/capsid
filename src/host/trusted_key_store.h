// WP-05 §9.5: TrustedKeyStore — the trusted Ed25519 public keys the Host
// verifies trusted-bytecode attestations against.
//
// Contract:
//   - key files are referenced by ABSOLUTE path and opened with
//     O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
//   - the file must be a regular file of exactly 32 raw bytes, owned by
//     root or the Host euid, with no group/other write bits;
//   - the file's dev/ino/size/mtime/ctime are verified before AND after
//     the read, so a swap mid-read is detected;
//   - the store OWNS every key's memory (std::string id + 32-byte array)
//     and hands out TrustedBytecodeKey views into it — no view ever points
//     at a temporary;
//   - key IDs are length-capped and the set is count-capped; duplicate
//     IDs are rejected;
//   - v1: the store is immutable after load; rotation happens on restart;
//   - a missing or invalid key fails closed with a STABLE error that never
//     contains the path or key material.

#ifndef CAPSID_HOST_TRUSTED_KEY_STORE_H
#define CAPSID_HOST_TRUSTED_KEY_STORE_H

#include "host/bytecode_attestation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace capsid::host {

inline constexpr std::size_t kMaxTrustedKeyIdBytes = 128;
inline constexpr std::size_t kMaxTrustedKeys = 64;

// A configured reference to a trusted key file (id → absolute path). The
// key bytes live on disk until load(); the descriptor is just the pointer.
struct TrustedKeyDescriptor {
    std::string key_id;
    std::string key_path;  // must be absolute
};

// Immutable (after load) key store. See the file comment for the contract.
class TrustedKeyStore {
public:
    // Loads every descriptor's key from disk with the §9.5 verification
    // contract. On any failure returns an empty store and a stable error.
    static TrustedKeyStore load(std::span<const TrustedKeyDescriptor> descriptors,
                                std::string* error);

    TrustedKeyStore() = default;

    // Views into THIS store's owned memory; valid for the store's life.
    std::span<const TrustedBytecodeKey> keys() const { return keys_; }

    std::size_t size() const { return keys_.size(); }

private:
    // Owned memory: the views in keys_ point here.
    std::vector<std::string> ids_;
    std::vector<std::array<std::uint8_t, kEd25519PublicKeyBytes>> raw_keys_;
    std::vector<TrustedBytecodeKey> keys_;
};

}  // namespace capsid::host

#endif
