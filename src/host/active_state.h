#ifndef CAPSID_HOST_ACTIVE_STATE_H
#define CAPSID_HOST_ACTIVE_STATE_H

#include <cstddef>
#include <string>
#include <string_view>

namespace capsid::host {

inline constexpr std::size_t kMaxActiveStateBytes = 16U * 1024U;
inline constexpr std::size_t kMaxActiveOperationIdBytes = 64U;
inline constexpr std::string_view kActiveStateSchema = "capsid-active-v1";
inline constexpr std::string_view kCrashBudgetExceededReason =
    "CRASH_BUDGET_EXCEEDED";

enum class ActiveServiceState {
    kActive,
    kRetired,
    kQuarantined,
};

// State-specific fields are exclusive:
// - active: version + generation
// - retired: previous_version + previous_generation
// - quarantined: version + generation + reason
// Every unused field must be empty.
struct ActiveStateDocument {
    ActiveServiceState state = ActiveServiceState::kActive;
    std::string application;
    std::string version;
    std::string generation;
    std::string previous_version;
    std::string previous_generation;
    std::string reason;
};

enum class ActiveStateErrorCode {
    kNone,
    kInvalidJson,
    kDuplicateKey,
    kUnknownField,
    kInvalidValue,
    kResourceLimit,
    kStorageError,
    kTemporaryFileExists,
    kGenerationNotComplete,
    // active.json has been renamed, but durability of that directory entry
    // was not established. The caller must not publish a new Registry
    // snapshot or report an ordinary pre-commit failure.
    kDurabilityUncertain,
};

struct ActiveStateError {
    ActiveStateErrorCode code = ActiveStateErrorCode::kNone;
    std::string path;
    std::string message;
};

struct ActiveStateDocumentResult {
    bool ok = false;
    ActiveStateDocument document;
    // Exact one-line JSON in schema/app/state/state-specific field order.
    std::string canonical_json;
    ActiveStateError error;
};

// Application IDs use [a-z0-9][a-z0-9._-]{0,62}; Version IDs use
// [A-Za-z0-9][A-Za-z0-9._-]{0,127}; generation is sha256: plus 64 lowercase
// hex digits. Unknown/duplicate fields and state-inapplicable fields fail
// closed. expected_application must match the document exactly.
ActiveStateDocumentResult parse_active_state_json(
    std::string_view expected_application,
    std::string_view json);

// Validates the same contract and returns its canonical JSON. It never emits
// a partially valid document.
ActiveStateDocumentResult encode_active_state_json(
    const ActiveStateDocument& document);

enum class ActiveStateIoStatus {
    kOk,
    kNotFound,
    kAlreadyExists,
    kError,
};

struct ActiveStateReadResult {
    ActiveStateIoStatus status = ActiveStateIoStatus::kError;
    std::string bytes;
};

enum class GenerationCompleteness {
    kComplete,
    kMissing,
    kIncomplete,
    kError,
};

// Logical filesystem boundary scoped to one already-validated App state
// directory. The POSIX adapter lands later; M0 uses this interface for
// deterministic ordering and crash/failure injection.
class ActiveStateFilesystem {
public:
    virtual ~ActiveStateFilesystem() = default;

    virtual ActiveStateIoStatus cleanup_stale_active_temps() = 0;
    virtual ActiveStateReadResult read_active_file() = 0;
    virtual GenerationCompleteness inspect_generation(
        std::string_view generation) = 0;

    virtual ActiveStateIoStatus create_active_temp_exclusive(
        std::string_view temp_name) = 0;
    virtual ActiveStateIoStatus write_active_temp(
        std::string_view temp_name,
        std::string_view bytes) = 0;
    virtual ActiveStateIoStatus sync_active_temp(
        std::string_view temp_name) = 0;
    virtual ActiveStateIoStatus rename_temp_over_active(
        std::string_view temp_name) = 0;
    virtual ActiveStateIoStatus sync_app_directory() = 0;
};

struct ActiveStatePersistResult {
    bool ok = false;
    // True once rename_temp_over_active() completed. If the following
    // directory sync fails, error is kDurabilityUncertain and callers must
    // reconcile rather than publish the new in-memory Registry state.
    bool active_name_replaced = false;
    std::string temp_name;
    ActiveStateError error;
};

// operation_id uses [A-Za-z0-9][A-Za-z0-9_-]{0,63}. An active document is
// written only if inspect_generation() reports kComplete. The fixed order is:
// create exclusive temp -> write all -> sync temp -> rename over active.json
// -> sync App directory. Success is returned only after the final sync.
ActiveStatePersistResult persist_active_state(
    const ActiveStateDocument& document,
    std::string_view operation_id,
    ActiveStateFilesystem& filesystem);

enum class ActiveStateRecoveryAction {
    kNone,
    kActivate,
    kKeepRetired,
    kKeepQuarantined,
};

struct ActiveStateRecoveryResult {
    bool ok = false;
    ActiveStateRecoveryAction action = ActiveStateRecoveryAction::kNone;
    ActiveStateDocument document;
    // Stale temp cleanup is best effort and never changes the selected state;
    // the caller must surface this flag as an operator warning.
    bool stale_temp_cleanup_failed = false;
    ActiveStateError error;
};

// Missing active.json is a successful kNone recovery. Active state is
// returned as kActivate only when the referenced generation is complete.
// Retired/quarantined states never inspect or start a generation. The
// filesystem interface intentionally exposes no generation listing API, so
// recovery cannot scan for or guess a "latest" generation.
ActiveStateRecoveryResult recover_active_state(
    std::string_view expected_application,
    ActiveStateFilesystem& filesystem);

}  // namespace capsid::host

#endif
