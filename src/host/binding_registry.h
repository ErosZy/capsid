// bindingsRoot security scan (Binding v1, docs/binding-technical-design.md
// §2.1). The Host scans the fixed directory layout once at startup and
// forms an immutable snapshot of every Binding package. The scan is the
// trust boundary between Host-managed files and the worker: it rejects
// symbolic links, hard links, FIFOs, sockets, device files, extra files,
// disallowed ownership, group/world-writable modes, oversized artifacts
// and invalid package names, and it validates every manifest against the
// binding manifest schema before any byte reaches a worker.

#ifndef CAPSID_HOST_BINDING_REGISTRY_H
#define CAPSID_HOST_BINDING_REGISTRY_H

#if defined(_WIN32)
#include <cstdint>
#else
#include <sys/types.h>
#endif

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

#if defined(_WIN32)
using BindingOwnerId = std::uint32_t;
// Owner allow-list predicate shared with the Windows registry regression:
// true for the process identity and, only when present in the process
// token, Administrators or SYSTEM. Broad groups (Everyone/Users/
// Authenticated Users) always return false.
bool binding_owner_is_trusted(const void* owner_sid);
#else
using BindingOwnerId = uid_t;
#endif

// Binding v1 committed-snapshot ceiling. Source is capped at 64 MiB per
// generation before JSON encoding; 160 MiB leaves room for JSON escaping,
// manifests, configs, permission lists, and framing while still bounding a
// recovery read before parsing.
inline constexpr std::size_t kMaxBindingsSnapshotBytes =
    160U * 1024U * 1024U;

// One validated Binding package, fully copied at scan time. The strings
// are private copies of the file bytes; later edits to bindingsRoot cannot
// change a returned snapshot (v1 does not watch or reload).
struct BindingPackageSnapshot {
    std::string id;             // the package directory name
    std::string manifest_json;  // validated manifest bytes
    std::string source;         // index.js bytes (bounded)
    std::string manifest_digest;  // canonical manifest digest
    std::string source_digest;    // raw source digest
};

struct BindingRegistrySnapshot {
    // Sorted by package id.
    std::vector<BindingPackageSnapshot> packages;
};

// Scans `root` and returns the immutable snapshot. `allowed_uids` is the
// owner allow set (production passes {0, geteuid()}); the root directory,
// every package directory and every file must be owned by one of them and
// must not be group- or world-writable. On failure `error` carries a static
// operator-facing diagnostic that names the offending path, never content.
bool scan_bindings_root(const std::string &root,
                        const std::vector<BindingOwnerId> &allowed_uids,
                        BindingRegistrySnapshot *out,
                        std::string *error);

// Deterministic race-injection seam used only by the registry regression
// tests. Production callers use scan_bindings_root() and cannot install a
// hook. The hook runs after an opened directory has been enumerated.
enum class BindingRegistryScanPhase {
    kRootEnumerated,
    kPackageEnumerated,
};
using BindingRegistryScanHook = std::function<void(
    BindingRegistryScanPhase phase, std::string_view package_id)>;
bool scan_bindings_root_with_test_hook(
    const std::string &root,
    const std::vector<BindingOwnerId> &allowed_uids,
    const BindingRegistryScanHook &hook,
    BindingRegistrySnapshot *out,
    std::string *error);

}  // namespace capsid::host

#endif
