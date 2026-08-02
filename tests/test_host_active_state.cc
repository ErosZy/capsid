#include "host/active_state.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using capsid::host::ActiveServiceState;
using capsid::host::ActiveStateDocument;
using capsid::host::ActiveStateDocumentResult;
using capsid::host::ActiveStateErrorCode;
using capsid::host::ActiveStateFilesystem;
using capsid::host::ActiveStateIoStatus;
using capsid::host::ActiveStatePersistResult;
using capsid::host::ActiveStateReadResult;
using capsid::host::ActiveStateRecoveryAction;
using capsid::host::ActiveStateRecoveryResult;
using capsid::host::GenerationCompleteness;
using capsid::host::encode_active_state_json;
using capsid::host::kCrashBudgetExceededReason;
using capsid::host::kMaxActiveStateBytes;
using capsid::host::parse_active_state_json;
using capsid::host::persist_active_state;
using capsid::host::recover_active_state;

constexpr std::string_view kGenerationOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kGenerationTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-active-state: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

ActiveStateDocument active_document(std::string_view version,
                                    std::string_view generation) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = "orders";
    document.version = std::string(version);
    document.generation = std::string(generation);
    return document;
}

ActiveStateDocument retired_document() {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kRetired;
    document.application = "orders";
    document.previous_version = "2026-07-31-002";
    document.previous_generation = std::string(kGenerationTwo);
    return document;
}

ActiveStateDocument quarantined_document() {
    ActiveStateDocument document =
        active_document("2026-07-31-002", kGenerationTwo);
    document.state = ActiveServiceState::kQuarantined;
    document.reason = std::string(kCrashBudgetExceededReason);
    return document;
}

std::string active_json(std::string_view version,
                        std::string_view generation) {
    return "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
           "\"state\":\"active\",\"version\":\"" +
           std::string(version) + "\",\"generation\":\"" +
           std::string(generation) + "\"}";
}

void require_document_error(const ActiveStateDocumentResult& result,
                            ActiveStateErrorCode code,
                            std::string_view path,
                            std::string_view label) {
    require(!result.ok, std::string(label) + " was accepted");
    require(result.error.code == code,
            std::string(label) + " returned the wrong error code");
    require(result.error.path == path,
            std::string(label) + " reported path '" + result.error.path +
                "' instead of '" + std::string(path) + "'");
    require(!result.error.message.empty(),
            std::string(label) + " returned no diagnostic");
    require(result.canonical_json.empty(),
            std::string(label) + " returned partial canonical JSON");
}

class FakeActiveStateFilesystem final : public ActiveStateFilesystem {
public:
    ActiveStateIoStatus cleanup_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus read_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus create_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus write_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus sync_temp_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus rename_status = ActiveStateIoStatus::kOk;
    ActiveStateIoStatus sync_directory_status = ActiveStateIoStatus::kOk;
    GenerationCompleteness default_generation =
        GenerationCompleteness::kMissing;
    std::map<std::string, GenerationCompleteness> generations;
    std::vector<std::string> calls;
    std::string visible_active;
    std::string durable_active;
    std::string temp_name;
    std::string temp_bytes;
    bool temp_exists = false;

    ActiveStateIoStatus cleanup_stale_active_temps() override {
        calls.emplace_back("cleanup-temps");
        if (cleanup_status == ActiveStateIoStatus::kOk) {
            temp_exists = false;
            temp_name.clear();
            temp_bytes.clear();
        }
        return cleanup_status;
    }

    ActiveStateReadResult read_active_file() override {
        calls.emplace_back("read-active");
        ActiveStateReadResult result;
        result.status = read_status;
        if (result.status == ActiveStateIoStatus::kOk) {
            result.bytes = visible_active;
        }
        return result;
    }

    GenerationCompleteness inspect_generation(
        std::string_view generation) override {
        calls.emplace_back("inspect:" + std::string(generation));
        const auto found = generations.find(std::string(generation));
        return found == generations.end() ? default_generation
                                          : found->second;
    }

    ActiveStateIoStatus create_active_temp_exclusive(
        std::string_view name) override {
        calls.emplace_back("create:" + std::string(name));
        if (create_status != ActiveStateIoStatus::kOk) {
            return create_status;
        }
        if (temp_exists) {
            return ActiveStateIoStatus::kAlreadyExists;
        }
        temp_exists = true;
        temp_name = std::string(name);
        temp_bytes.clear();
        return ActiveStateIoStatus::kOk;
    }

    ActiveStateIoStatus write_active_temp(
        std::string_view name,
        std::string_view bytes) override {
        calls.emplace_back("write:" + std::string(name));
        if (write_status != ActiveStateIoStatus::kOk) {
            return write_status;
        }
        if (!temp_exists || name != temp_name) {
            return ActiveStateIoStatus::kError;
        }
        temp_bytes = std::string(bytes);
        return ActiveStateIoStatus::kOk;
    }

    ActiveStateIoStatus sync_active_temp(std::string_view name) override {
        calls.emplace_back("sync-temp:" + std::string(name));
        if (!temp_exists || name != temp_name) {
            return ActiveStateIoStatus::kError;
        }
        return sync_temp_status;
    }

    ActiveStateIoStatus rename_temp_over_active(
        std::string_view name) override {
        calls.emplace_back("rename:" + std::string(name) + "->active.json");
        if (rename_status != ActiveStateIoStatus::kOk) {
            return rename_status;
        }
        if (!temp_exists || name != temp_name) {
            return ActiveStateIoStatus::kError;
        }
        visible_active = temp_bytes;
        temp_exists = false;
        temp_name.clear();
        temp_bytes.clear();
        return ActiveStateIoStatus::kOk;
    }

    ActiveStateIoStatus sync_app_directory() override {
        calls.emplace_back("sync-directory");
        if (sync_directory_status == ActiveStateIoStatus::kOk) {
            durable_active = visible_active;
        }
        return sync_directory_status;
    }

    void set_durable_active(std::string bytes) {
        durable_active = std::move(bytes);
        visible_active = durable_active;
        read_status = ActiveStateIoStatus::kOk;
    }

    // A crash before directory fsync may recover either the old or the new
    // atomic name. It may never expose partial temp bytes as active.json.
    void crash(bool retain_unsynced_rename) {
        if (retain_unsynced_rename) {
            durable_active = visible_active;
        } else {
            visible_active = durable_active;
        }
        visible_active = durable_active;
        calls.clear();
    }
};

void require_recovery_error(const ActiveStateRecoveryResult& result,
                            ActiveStateErrorCode code,
                            std::string_view path,
                            std::string_view label) {
    require(!result.ok, std::string(label) + " recovered successfully");
    require(result.action == ActiveStateRecoveryAction::kNone,
            std::string(label) + " selected a service state");
    require(result.error.code == code && result.error.path == path &&
                !result.error.message.empty(),
            std::string(label) + " returned the wrong recovery error");
}

void test_state_documents_are_strict_and_canonical() {
    const std::string expected_active =
        active_json("2026-07-31-002", kGenerationTwo);
    const ActiveStateDocumentResult active = parse_active_state_json(
        "orders",
        " { \"generation\" : \"" + std::string(kGenerationTwo) +
            "\", \"version\":\"2026-07-31-002\","
            "\"state\":\"active\",\"app\":\"orders\","
            "\"schema\":\"capsid-active-v1\" }");
    require(active.ok && active.error.code == ActiveStateErrorCode::kNone &&
                active.error.path.empty() && active.error.message.empty(),
            "valid active state was rejected");
    require(active.document.state == ActiveServiceState::kActive &&
                active.document.application == "orders" &&
                active.document.version == "2026-07-31-002" &&
                active.document.generation == kGenerationTwo &&
                active.document.previous_version.empty() &&
                active.document.previous_generation.empty() &&
                active.document.reason.empty() &&
                active.canonical_json == expected_active,
            "active state was not normalized exactly");

    const std::string expected_retired =
        "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
        "\"state\":\"retired\","
        "\"previousVersion\":\"2026-07-31-002\","
        "\"previousGeneration\":\"" + std::string(kGenerationTwo) +
        "\"}";
    const ActiveStateDocumentResult retired =
        encode_active_state_json(retired_document());
    require(retired.ok && retired.canonical_json == expected_retired,
            "retired tombstone is not canonical");
    const ActiveStateDocumentResult parsed_retired =
        parse_active_state_json("orders", expected_retired);
    require(parsed_retired.ok &&
                parsed_retired.document.state == ActiveServiceState::kRetired &&
                parsed_retired.document.previous_version ==
                    "2026-07-31-002" &&
                parsed_retired.document.previous_generation == kGenerationTwo,
            "retired tombstone did not round-trip");

    const std::string expected_quarantined =
        "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
        "\"state\":\"quarantined\","
        "\"version\":\"2026-07-31-002\","
        "\"generation\":\"" + std::string(kGenerationTwo) +
        "\",\"reason\":\"CRASH_BUDGET_EXCEEDED\"}";
    const ActiveStateDocumentResult quarantined =
        encode_active_state_json(quarantined_document());
    require(quarantined.ok &&
                quarantined.canonical_json == expected_quarantined,
            "quarantined state is not canonical");
    const ActiveStateDocumentResult parsed_quarantined =
        parse_active_state_json("orders", expected_quarantined);
    require(parsed_quarantined.ok &&
                parsed_quarantined.document.state ==
                    ActiveServiceState::kQuarantined &&
                parsed_quarantined.document.reason ==
                    kCrashBudgetExceededReason,
            "quarantined state did not round-trip");
}

void test_state_schema_fails_closed() {
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"state\":\"retired\","
            "\"version\":\"v1\",\"generation\":\"" +
                std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kDuplicateKey, "", "duplicate state field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) +
                "\",\"mystery\":true}"),
        ActiveStateErrorCode::kUnknownField, "/mystery", "unknown field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v2\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/schema",
        "unsupported state schema");
    require_document_error(
        parse_active_state_json(
            "billing",
            active_json("2026-07-31-002", kGenerationTwo)),
        ActiveStateErrorCode::kInvalidValue, "/app",
        "cross-App active pointer");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"Orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/app", "invalid App ID");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"paused\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/state", "unknown state");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"../v1\","
            "\"generation\":\"" + std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/version", "invalid Version ID");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"sha256:ABC\"}"),
        ActiveStateErrorCode::kInvalidValue, "/generation",
        "invalid generation digest");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\"}"),
        ActiveStateErrorCode::kInvalidValue, "/generation",
        "active state without generation");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) +
                "\",\"previousVersion\":\"v0\"}"),
        ActiveStateErrorCode::kInvalidValue, "/previousVersion",
        "active state with tombstone field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) +
                "\",\"previousVersion\":\"\"}"),
        ActiveStateErrorCode::kInvalidValue, "/previousVersion",
        "active state with empty tombstone field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"retired\","
            "\"previousGeneration\":\"" + std::string(kGenerationOne) +
                "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/previousVersion",
        "retired state without previous Version");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"retired\",\"version\":\"\","
            "\"previousVersion\":\"v1\",\"previousGeneration\":\"" +
                std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/version",
        "retired state with empty active field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"quarantined\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) +
                "\",\"reason\":\"UNKNOWN\"}"),
        ActiveStateErrorCode::kInvalidValue, "/reason",
        "unknown quarantine reason");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"quarantined\",\"version\":\"v1\","
            "\"generation\":\"" + std::string(kGenerationOne) +
                "\",\"reason\":\"CRASH_BUDGET_EXCEEDED\","
                "\"previousGeneration\":\"\"}"),
        ActiveStateErrorCode::kInvalidValue, "/previousGeneration",
        "quarantined state with empty tombstone field");
    require_document_error(
        parse_active_state_json(
            "orders",
            "{\"schema\":\"capsid-active-v1\","
            "\"app\":\"orders\\u0000shadow\",\"state\":\"active\","
            "\"version\":\"v1\",\"generation\":\"" +
                std::string(kGenerationOne) + "\"}"),
        ActiveStateErrorCode::kInvalidValue, "/app", "NUL in state value");
    require_document_error(
        parse_active_state_json("orders", "[]"),
        ActiveStateErrorCode::kInvalidJson, "", "non-object state root");
    require_document_error(
        parse_active_state_json("orders", std::string(kMaxActiveStateBytes + 1,
                                                       'x')),
        ActiveStateErrorCode::kResourceLimit, "", "oversized active state");

    ActiveStateDocument invalid = active_document("v1", kGenerationOne);
    invalid.previous_version = "v0";
    require_document_error(
        encode_active_state_json(invalid),
        ActiveStateErrorCode::kInvalidValue, "/previousVersion",
        "encoder state-inapplicable field");
}

void test_atomic_persist_sequence_and_errors() {
    const ActiveStateDocument document =
        active_document("2026-07-31-002", kGenerationTwo);
    const std::string expected_json =
        active_json("2026-07-31-002", kGenerationTwo);
    const std::string expected_temp = "active.json.tmp.op_01";

    FakeActiveStateFilesystem success;
    success.generations[std::string(kGenerationTwo)] =
        GenerationCompleteness::kComplete;
    const ActiveStatePersistResult persisted =
        persist_active_state(document, "op_01", success);
    const std::vector<std::string> expected_calls{
        "inspect:" + std::string(kGenerationTwo),
        "create:" + expected_temp,
        "write:" + expected_temp,
        "sync-temp:" + expected_temp,
        "rename:" + expected_temp + "->active.json",
        "sync-directory",
    };
    require(persisted.ok && persisted.active_name_replaced &&
                persisted.temp_name == expected_temp &&
                persisted.error.code == ActiveStateErrorCode::kNone &&
                success.calls == expected_calls &&
                success.visible_active == expected_json &&
                success.durable_active == expected_json,
            "active state persistence did not follow the frozen sync order");

    FakeActiveStateFilesystem invalid_operation;
    const ActiveStatePersistResult invalid_operation_result =
        persist_active_state(document, "../op", invalid_operation);
    require(!invalid_operation_result.ok &&
                !invalid_operation_result.active_name_replaced &&
                invalid_operation_result.error.code ==
                    ActiveStateErrorCode::kInvalidValue &&
                invalid_operation_result.error.path == "/operationId" &&
                invalid_operation.calls.empty(),
            "invalid operation ID reached the filesystem");

    FakeActiveStateFilesystem incomplete;
    incomplete.generations[std::string(kGenerationTwo)] =
        GenerationCompleteness::kIncomplete;
    const ActiveStatePersistResult incomplete_result =
        persist_active_state(document, "op_02", incomplete);
    require(!incomplete_result.ok &&
                !incomplete_result.active_name_replaced &&
                incomplete_result.error.code ==
                    ActiveStateErrorCode::kGenerationNotComplete &&
                incomplete_result.error.path == "/generation" &&
                incomplete.calls == std::vector<std::string>{
                    "inspect:" + std::string(kGenerationTwo)},
            "incomplete generation reached active.json persistence");

    FakeActiveStateFilesystem inspect_error;
    inspect_error.generations[std::string(kGenerationTwo)] =
        GenerationCompleteness::kError;
    const ActiveStatePersistResult inspect_error_result =
        persist_active_state(document, "op_inspect", inspect_error);
    require(!inspect_error_result.ok &&
                !inspect_error_result.active_name_replaced &&
                inspect_error_result.error.code ==
                    ActiveStateErrorCode::kStorageError &&
                inspect_error_result.error.path == "/generation" &&
                inspect_error.calls == std::vector<std::string>{
                    "inspect:" + std::string(kGenerationTwo)},
            "generation inspection I/O error was reported as incompleteness");

    FakeActiveStateFilesystem create_collision;
    create_collision.generations[std::string(kGenerationTwo)] =
        GenerationCompleteness::kComplete;
    create_collision.create_status = ActiveStateIoStatus::kAlreadyExists;
    const ActiveStatePersistResult collision =
        persist_active_state(document, "op_03", create_collision);
    require(!collision.ok && !collision.active_name_replaced &&
                collision.error.code ==
                    ActiveStateErrorCode::kTemporaryFileExists &&
                create_collision.calls.size() == 2,
            "exclusive temp collision was not fail-closed");

    struct FailureCase {
        enum class Point { kCreate, kWrite, kSyncTemp, kRename, kSyncDirectory };
        Point point;
        std::size_t expected_calls;
        bool replaced;
        ActiveStateErrorCode code;
    };
    const FailureCase cases[] = {
        {FailureCase::Point::kCreate, 2, false,
         ActiveStateErrorCode::kStorageError},
        {FailureCase::Point::kWrite, 3, false,
         ActiveStateErrorCode::kStorageError},
        {FailureCase::Point::kSyncTemp, 4, false,
         ActiveStateErrorCode::kStorageError},
        {FailureCase::Point::kRename, 5, false,
         ActiveStateErrorCode::kStorageError},
        {FailureCase::Point::kSyncDirectory, 6, true,
         ActiveStateErrorCode::kDurabilityUncertain},
    };
    for (const FailureCase& test : cases) {
        FakeActiveStateFilesystem filesystem;
        filesystem.generations[std::string(kGenerationTwo)] =
            GenerationCompleteness::kComplete;
        switch (test.point) {
        case FailureCase::Point::kCreate:
            filesystem.create_status = ActiveStateIoStatus::kError;
            break;
        case FailureCase::Point::kWrite:
            filesystem.write_status = ActiveStateIoStatus::kError;
            break;
        case FailureCase::Point::kSyncTemp:
            filesystem.sync_temp_status = ActiveStateIoStatus::kError;
            break;
        case FailureCase::Point::kRename:
            filesystem.rename_status = ActiveStateIoStatus::kError;
            break;
        case FailureCase::Point::kSyncDirectory:
            filesystem.sync_directory_status = ActiveStateIoStatus::kError;
            break;
        }
        const ActiveStatePersistResult result =
            persist_active_state(document, "op_04", filesystem);
        require(!result.ok &&
                    result.active_name_replaced == test.replaced &&
                    result.error.code == test.code &&
                    filesystem.calls.size() == test.expected_calls,
                "filesystem failure crossed a persistence boundary");
    }

    FakeActiveStateFilesystem retired;
    const ActiveStatePersistResult retired_result =
        persist_active_state(retired_document(), "op_retire", retired);
    require(retired_result.ok && !retired.calls.empty() &&
                retired.calls[0] == "create:active.json.tmp.op_retire",
            "retired tombstone incorrectly required a generation lookup");

    FakeActiveStateFilesystem quarantined;
    const ActiveStatePersistResult quarantined_result = persist_active_state(
        quarantined_document(), "op_quarantine", quarantined);
    require(quarantined_result.ok && !quarantined.calls.empty() &&
                quarantined.calls[0] ==
                    "create:active.json.tmp.op_quarantine",
            "quarantined state incorrectly required a generation lookup");
}

void test_recovery_never_selects_an_incomplete_generation() {
    FakeActiveStateFilesystem missing_state;
    missing_state.read_status = ActiveStateIoStatus::kNotFound;
    const ActiveStateRecoveryResult absent =
        recover_active_state("orders", missing_state);
    require(absent.ok && absent.action == ActiveStateRecoveryAction::kNone &&
                absent.error.code == ActiveStateErrorCode::kNone &&
                missing_state.calls == std::vector<std::string>{
                    "cleanup-temps", "read-active"},
            "missing active.json did not recover as an absent App");

    FakeActiveStateFilesystem complete;
    complete.set_durable_active(active_json("v1", kGenerationOne));
    complete.generations[std::string(kGenerationOne)] =
        GenerationCompleteness::kComplete;
    const ActiveStateRecoveryResult active =
        recover_active_state("orders", complete);
    require(active.ok &&
                active.action == ActiveStateRecoveryAction::kActivate &&
                active.document.generation == kGenerationOne,
            "complete active generation was not recovered");

    for (const GenerationCompleteness status : {
             GenerationCompleteness::kMissing,
             GenerationCompleteness::kIncomplete}) {
        FakeActiveStateFilesystem filesystem;
        filesystem.set_durable_active(active_json("v1", kGenerationOne));
        filesystem.generations[std::string(kGenerationOne)] = status;
        require_recovery_error(
            recover_active_state("orders", filesystem),
            ActiveStateErrorCode::kGenerationNotComplete, "/generation",
            "active pointer to missing/incomplete generation");
    }

    FakeActiveStateFilesystem generation_error;
    generation_error.set_durable_active(active_json("v1", kGenerationOne));
    generation_error.generations[std::string(kGenerationOne)] =
        GenerationCompleteness::kError;
    require_recovery_error(
        recover_active_state("orders", generation_error),
        ActiveStateErrorCode::kStorageError, "/generation",
        "generation inspection I/O failure");

    FakeActiveStateFilesystem malformed;
    malformed.set_durable_active("{not-json");
    require_recovery_error(
        recover_active_state("orders", malformed),
        ActiveStateErrorCode::kInvalidJson, "", "malformed active state");
    require(malformed.calls == std::vector<std::string>{
                "cleanup-temps", "read-active"},
            "malformed active state triggered generation inspection");

    FakeActiveStateFilesystem retired;
    retired.set_durable_active(
        encode_active_state_json(retired_document()).canonical_json);
    const ActiveStateRecoveryResult retired_result =
        recover_active_state("orders", retired);
    require(retired_result.ok &&
                retired_result.action ==
                    ActiveStateRecoveryAction::kKeepRetired &&
                retired.calls == std::vector<std::string>{
                    "cleanup-temps", "read-active"},
            "retired App attempted to recover a worker pool");

    FakeActiveStateFilesystem quarantined;
    quarantined.set_durable_active(
        encode_active_state_json(quarantined_document()).canonical_json);
    const ActiveStateRecoveryResult quarantined_result =
        recover_active_state("orders", quarantined);
    require(quarantined_result.ok &&
                quarantined_result.action ==
                    ActiveStateRecoveryAction::kKeepQuarantined &&
                quarantined.calls == std::vector<std::string>{
                    "cleanup-temps", "read-active"},
            "quarantined App attempted to recover a worker pool");

    FakeActiveStateFilesystem cleanup_warning;
    cleanup_warning.cleanup_status = ActiveStateIoStatus::kError;
    cleanup_warning.set_durable_active(active_json("v1", kGenerationOne));
    cleanup_warning.generations[std::string(kGenerationOne)] =
        GenerationCompleteness::kComplete;
    const ActiveStateRecoveryResult warning =
        recover_active_state("orders", cleanup_warning);
    require(warning.ok && warning.stale_temp_cleanup_failed &&
                warning.action == ActiveStateRecoveryAction::kActivate,
            "best-effort stale temp cleanup blocked valid recovery");

    FakeActiveStateFilesystem read_error;
    read_error.read_status = ActiveStateIoStatus::kError;
    require_recovery_error(
        recover_active_state("orders", read_error),
        ActiveStateErrorCode::kStorageError, "", "active.json read failure");
}

void test_crash_boundaries_recover_old_or_complete_new_only() {
    const std::string old_json = active_json("v1", kGenerationOne);
    const ActiveStateDocument next = active_document("v2", kGenerationTwo);

    struct CrashCase {
        enum class Point { kCreate, kWrite, kSyncTemp, kRename };
        Point point;
    };
    for (const CrashCase test : {
             CrashCase{CrashCase::Point::kCreate},
             CrashCase{CrashCase::Point::kWrite},
             CrashCase{CrashCase::Point::kSyncTemp},
             CrashCase{CrashCase::Point::kRename}}) {
        FakeActiveStateFilesystem filesystem;
        filesystem.set_durable_active(old_json);
        filesystem.generations[std::string(kGenerationOne)] =
            GenerationCompleteness::kComplete;
        filesystem.generations[std::string(kGenerationTwo)] =
            GenerationCompleteness::kComplete;
        switch (test.point) {
        case CrashCase::Point::kCreate:
            filesystem.create_status = ActiveStateIoStatus::kError;
            break;
        case CrashCase::Point::kWrite:
            filesystem.write_status = ActiveStateIoStatus::kError;
            break;
        case CrashCase::Point::kSyncTemp:
            filesystem.sync_temp_status = ActiveStateIoStatus::kError;
            break;
        case CrashCase::Point::kRename:
            filesystem.rename_status = ActiveStateIoStatus::kError;
            break;
        }
        require(!persist_active_state(next, "crash_01", filesystem).ok,
                "injected pre-rename failure reported success");
        filesystem.crash(false);
        const ActiveStateRecoveryResult recovered =
            recover_active_state("orders", filesystem);
        require(recovered.ok &&
                    recovered.action == ActiveStateRecoveryAction::kActivate &&
                    recovered.document.generation == kGenerationOne,
                "pre-rename crash did not preserve the old active state");
    }

    // rename is atomic, but without parent fsync a crash may retain either
    // directory entry. Both outcomes must parse and reference a COMPLETE
    // generation; no temp bytes may become active.json.
    for (const bool retain_new_name : {false, true}) {
        FakeActiveStateFilesystem filesystem;
        filesystem.set_durable_active(old_json);
        filesystem.generations[std::string(kGenerationOne)] =
            GenerationCompleteness::kComplete;
        filesystem.generations[std::string(kGenerationTwo)] =
            GenerationCompleteness::kComplete;
        filesystem.sync_directory_status = ActiveStateIoStatus::kError;
        const ActiveStatePersistResult uncertain =
            persist_active_state(next, "crash_02", filesystem);
        require(!uncertain.ok && uncertain.active_name_replaced &&
                    uncertain.error.code ==
                        ActiveStateErrorCode::kDurabilityUncertain,
                "post-rename directory failure was not marked uncertain");
        filesystem.crash(retain_new_name);
        const ActiveStateRecoveryResult recovered =
            recover_active_state("orders", filesystem);
        const std::string_view expected =
            retain_new_name ? kGenerationTwo : kGenerationOne;
        require(recovered.ok &&
                    recovered.action == ActiveStateRecoveryAction::kActivate &&
                    recovered.document.generation == expected,
                "post-rename crash selected neither complete old nor new state");
    }

    FakeActiveStateFilesystem committed;
    committed.set_durable_active(old_json);
    committed.generations[std::string(kGenerationOne)] =
        GenerationCompleteness::kComplete;
    committed.generations[std::string(kGenerationTwo)] =
        GenerationCompleteness::kComplete;
    require(persist_active_state(next, "commit_01", committed).ok,
            "fully synced active state did not commit");
    committed.crash(false);
    const ActiveStateRecoveryResult recovered =
        recover_active_state("orders", committed);
    require(recovered.ok &&
                recovered.action == ActiveStateRecoveryAction::kActivate &&
                recovered.document.generation == kGenerationTwo,
            "post-fsync crash lost the committed active state");
}

}  // namespace

int main() {
    test_state_documents_are_strict_and_canonical();
    test_state_schema_fails_closed();
    test_atomic_persist_sequence_and_errors();
    test_recovery_never_selects_an_incomplete_generation();
    test_crash_boundaries_recover_old_or_complete_new_only();
    return 0;
}
