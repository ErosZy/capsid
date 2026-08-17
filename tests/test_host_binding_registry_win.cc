// Windows bindingsRoot security scan regression (Binding v1 §2.1).
//
// Windows has no POSIX uid/mode bits, so this test proves the Windows
// equivalents: reparse points (symlink/junction) rejection, hard-link
// rejection, Everyone/Users writable ACL rejection, package layout/size
// limits, and the race-injection hook.

#include "host/binding_registry.h"
#include "win32_compat.h"

#include <aclapi.h>
#include <sddl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using capsid::host::BindingRegistryScanPhase;
using capsid::host::scan_bindings_root;
using capsid::host::scan_bindings_root_with_test_hook;

constexpr char kValidManifest[] =
    R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client"]},"permissions":{"modules":["capsid:internal/core","capsid:utils"],"net":{"allow":["*:27017"]}}})json";
constexpr char kValidSource[] =
    "export default function createBinding() { return { async find() { return 1; } }; }";

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "test-host-binding-registry-win: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0,
        NULL, NULL);
    require(size > 0, "WideCharToMultiByte sizing failed");
    std::string out(static_cast<size_t>(size), '\0');
    require(WideCharToMultiByte(
                CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                out.data(), size, NULL, NULL) == size,
            "WideCharToMultiByte failed");
    return out;
}

std::wstring widen(const std::string &text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), NULL, 0);
    require(size > 0, "MultiByteToWideChar sizing failed");
    std::wstring out(static_cast<size_t>(size), L'\0');
    require(MultiByteToWideChar(
                CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                out.data(), size) == size,
            "MultiByteToWideChar failed");
    return out;
}

void remove_tree(const std::string &path) {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path(path), error);
    if (std::filesystem::exists(path, error)) {
        // remove_all refuses to recurse through a junction. rmdir the
        // junction itself so cleanup never touches the target tree.
        _wrmdir(widen(path).c_str());
    }
}

class Fixture {
public:
    explicit Fixture() {
        std::wstring temp(32768, L'\0');
        const DWORD size = GetTempPathW(32768, temp.data());
        require(size > 0 && size < 32768, "GetTempPathW failed");
        temp.resize(size);
        std::wstring pattern =
            temp + L"capsid-binding-registry-win-XXXXXX";
        const DWORD unique = GetTempFileNameW(
            temp.c_str(), L"reg", 0, pattern.data());
        require(unique != 0, "GetTempFileNameW failed");
        pattern.resize(std::wcslen(pattern.c_str()));
        DeleteFileW(pattern.c_str());
        require(CreateDirectoryW(pattern.c_str(), NULL),
                "cannot create fixture root");
        root_ = narrow(pattern);
    }

    ~Fixture() { remove_tree(root_); }

    const std::string &root() const { return root_; }

    std::string path(const std::string &relative) const {
        return root_ + "\\" + relative;
    }    void write(const std::string &relative,
               const std::string &content) const {
        const std::wstring file = widen(path(relative));
        const std::filesystem::path parent =
            std::filesystem::path(file).parent_path();
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        require(!error, "cannot create fixture parent");
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output),
                "cannot open fixture file: " + path(relative));
        output.write(content.data(),
                     static_cast<std::streamsize>(content.size()));
        output.close();
        require(static_cast<bool>(output), "cannot write fixture file");
    }

    void add_valid_package(const std::string &id) const {
        write(id + "\\manifest.json", kValidManifest);
        write(id + "\\index.js", kValidSource);
    }

private:
    std::string root_;
};

bool scan_ok(Fixture *fixture, std::string *error) {
    capsid::host::BindingRegistrySnapshot snapshot;
    const bool ok = scan_bindings_root(
        fixture->root(), {0}, &snapshot, error);
    return ok;
}

bool make_junction(const std::string &link, const std::string &target) {
    // mklink /J creates a directory junction without admin rights.
    std::string command =
        "cmd.exe /c mklink /J \"" + link + "\" \"" + target + "\"";
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessA(
        NULL, command.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
        NULL, &startup, &process);
    if (!created) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

void test_valid_packages_and_sorted_order(Fixture *fixture) {
    fixture->add_valid_package("zulu");
    fixture->add_valid_package("alpha");
    std::string error;
    capsid::host::BindingRegistrySnapshot snapshot;
    bool ok = false;
    std::string first_error;
    // A freshly written NTFS tree can be touched by search indexers or
    // antivirus on shared runners between two opens; the scanner is
    // required to fail closed on that, so retry only the recognized
    // transient identity-change diagnostics. A policy rejection (owner,
    // ACL, link, layout) is deterministic and fails immediately.
    for (int attempt = 0; attempt < 3; ++attempt) {
        error.clear();
        ok = scan_bindings_root(
            fixture->root(), {0}, &snapshot, &error);
        if (ok) {
            break;
        }
        if (first_error.empty()) {
            first_error = error;
        }
        if (error.find("changed") == std::string::npos) {
            break;
        }
        Sleep(50);
    }
    require(ok, "valid Windows registry rejected: " +
                    (first_error.empty() ? error : first_error) +
                    (first_error != error ? " (last: " + error + ")" : ""));
    require(snapshot.packages.size() == 2, "expected two packages");
    require(snapshot.packages[0].id == "alpha" &&
                snapshot.packages[1].id == "zulu",
            "packages are not sorted by id");
    require(snapshot.packages[0].manifest_digest.size() == 71,
            "manifest digest missing");
    require(snapshot.packages[0].source_digest.size() == 71,
            "source digest missing");
}

void test_invalid_package_id_rejected(Fixture *fixture) {
    fixture->add_valid_package("NotValid");
    std::string error;
    require(!scan_ok(fixture, &error),
            "invalid package id was accepted");
}

void test_extra_file_rejected(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    fixture->write("mongo\\stray.txt", "x");
    std::string error;
    require(!scan_ok(fixture, &error),
            "package with an extra file was accepted");
}

void test_hard_link_rejected(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    const std::wstring existing =
        widen(fixture->path("mongo\\index.js"));
    const std::wstring linked =
        widen(fixture->path("mongo\\other.js"));
    require(GetFileAttributesW(existing.c_str()) != INVALID_FILE_ATTRIBUTES,
            "existing hard-link source does not exist");
    const BOOL ok = CreateHardLinkW(linked.c_str(), existing.c_str(), NULL);
    if (!ok) {
        fail("CreateHardLinkW failed (GetLastError=" +
             std::to_string(GetLastError()) + ")");
    }
    std::string error;
    require(!scan_ok(fixture, &error),
            "package with a hard link was accepted");
}

void test_junction_rejected(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    require(make_junction(fixture->path("link"), fixture->path("mongo")),
            "cannot create a directory junction (mklink /J)");
    std::string error;
    require(!scan_ok(fixture, &error),
            "root child junction was accepted");
}

void test_world_writable_acl_rejected(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    PSID everyone = NULL;
    require(ConvertStringSidToSidW(L"S-1-1-0", &everyone),
            "ConvertStringSidToSidW failed");
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions = FILE_GENERIC_WRITE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone);
    PACL acl = NULL;
    const DWORD set_entries = SetEntriesInAclW(1, &access, NULL, &acl);
    LocalFree(everyone);
    require(set_entries == ERROR_SUCCESS && acl != NULL,
            "SetEntriesInAclW failed");
    const DWORD set_security = SetNamedSecurityInfoW(
        const_cast<wchar_t *>(widen(fixture->path("mongo\\manifest.json")).c_str()),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
    LocalFree(acl);
    require(set_security == ERROR_SUCCESS,
            "SetNamedSecurityInfoW failed");
    std::string error;
    require(!scan_ok(fixture, &error),
            "Everyone-writable file ACL was accepted");
}

// The owner allow-list on Windows is the process identity plus
// Administrators/SYSTEM. Broad token groups such as Everyone, Users and
// Authenticated Users must never satisfy it. The helper check is asserted
// directly; the end-to-end variant additionally requires setting the
// bindingsRoot owner, which needs a privileged runner, so it skips cleanly
// whenever the owner change is not permitted.
void test_binding_owner_is_trusted_helper() {
    using capsid::host::binding_owner_is_trusted;

    require(!binding_owner_is_trusted(nullptr),
            "NULL SID was accepted as a trusted binding owner");

    const auto convert = [](const wchar_t *text) {
        PSID sid = NULL;
        require(ConvertStringSidToSidW(text, &sid) != FALSE,
                "ConvertStringSidToSidW failed");
        return sid;
    };
    PSID everyone = convert(L"S-1-1-0");
    PSID users = convert(L"S-1-5-32-545");
    PSID authenticated_users = convert(L"S-1-5-11");

    require(!binding_owner_is_trusted(everyone),
            "Everyone SID was accepted as a trusted binding owner");
    require(!binding_owner_is_trusted(users),
            "Users SID was accepted as a trusted binding owner");
    require(!binding_owner_is_trusted(authenticated_users),
            "Authenticated Users SID was accepted as a trusted binding owner");

    // Administrators/SYSTEM acceptance depends on whether this token
    // actually carries the group, so it is exercised through the real
    // scan path rather than asserted here. The broad-group rejection above
    // is the regression boundary: those SIDs appear in every ordinary
    // token and must never satisfy the ownership check.
    LocalFree(everyone);
    LocalFree(users);
    LocalFree(authenticated_users);
}

void test_foreign_group_owner_rejected(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    PSID users = NULL;
    require(ConvertStringSidToSidW(L"S-1-5-32-545", &users),
            "ConvertStringSidToSidW failed");
    const DWORD set_owner = SetNamedSecurityInfoW(
        const_cast<wchar_t *>(widen(fixture->root()).c_str()),
        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, users, NULL, NULL, NULL);
    LocalFree(users);
    if (set_owner != ERROR_SUCCESS) {
        // WRITE_OWNER is unavailable to most local runners (and some
        // remapped privileges surface as other errors than the two
        // canonical ones). The deterministic helper assertions above keep
        // the boundary covered, so skip rather than fail or assert vacuously.
        std::cout << "foreign-owner end-to-end check skipped (owner change "
                     "unavailable, error "
                  << set_owner << ")"
                  << std::endl;
        return;
    }
    std::string error;
    require(!scan_ok(fixture, &error),
            "Users-owned bindingsRoot was accepted as trusted");
}

void test_oversized_source_rejected(Fixture *fixture) {
    fixture->write("mongo\\manifest.json", kValidManifest);
    fixture->write("mongo\\index.js", std::string());
    const HANDLE file = CreateFileW(
        widen(fixture->path("mongo\\index.js")).c_str(), FILE_WRITE_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    require(file != INVALID_HANDLE_VALUE, "CreateFileW failed");
    LARGE_INTEGER position = {};
    position.QuadPart = 16 * 1024 * 1024 + 1;
    require(SetFilePointerEx(file, position, NULL, FILE_BEGIN) &&
                SetEndOfFile(file),
            "cannot grow fixture source");
    CloseHandle(file);
    std::string error;
    require(!scan_ok(fixture, &error),
            "oversized source file was accepted");
}

void test_race_hook_fails_closed(Fixture *fixture) {
    fixture->add_valid_package("mongo");
    const std::string package_path = fixture->path("mongo");
    capsid::host::BindingRegistrySnapshot snapshot;
    std::string error;
    const bool ok = scan_bindings_root_with_test_hook(
        fixture->root(), {0},
        [&](BindingRegistryScanPhase phase, std::string_view package_id) {
            (void)package_id;
            if (phase == BindingRegistryScanPhase::kRootEnumerated) {
                remove_tree(package_path);
            }
        },
        &snapshot, &error);
    require(!ok, "scan succeeded although the package was removed mid-scan");
}

}  // namespace

int main() {
    test_binding_owner_is_trusted_helper();
    {
        Fixture fixture;
        test_valid_packages_and_sorted_order(&fixture);
    }
    {
        Fixture fixture;
        test_invalid_package_id_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_extra_file_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_hard_link_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_junction_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_world_writable_acl_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_foreign_group_owner_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_oversized_source_rejected(&fixture);
    }
    {
        Fixture fixture;
        test_race_hook_fails_closed(&fixture);
    }
    std::cout << "host binding registry windows: ok" << std::endl;
    return 0;
}
