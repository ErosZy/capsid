// Frozen RED: host_artifact_safe_read (M1D).
//
// Covers the deployment-input safe-read boundary:
//   - a legal source-only version reads fully with identity;
//   - a legal trusted-bytecode version (all three files) reads fully;
//   - symlink, FIFO, device, socket, directory and hostile paths reject;
//   - over-limit files reject;
//   - an in-place replacement / mid-read truncation fails as an identity
//     change;
//   - a partial bytecode triple (bundle.qjsb without the attestation or
//     signature) rejects all-or-none;
//   - error messages never contain file content.

#include "host/artifact_safe_read.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
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

struct Fixture {
    std::string root;
    int root_fd = -1;
};

Fixture make_fixture(const char* app, const char* version) {
    Fixture fixture;
    fixture.root = "/tmp/capsid-safe-read-XXXXXX";
    char* dir = mkdtemp(&fixture.root[0]);
    require(dir != nullptr, "cannot create fixture root");
    fixture.root = dir;
    fixture.root_fd = open(fixture.root.c_str(), O_RDONLY | O_DIRECTORY);
    require(fixture.root_fd >= 0, "cannot open fixture root");
    require(mkdirat(fixture.root_fd, app, 0700) == 0,
            "cannot create app dir");
    const std::string version_dir = std::string(app) + "/" + version;
    require(mkdirat(fixture.root_fd, version_dir.c_str(), 0700) == 0,
            "cannot create version dir");
    return fixture;
}

void require_error_code(capsid::host::SafeReadErrorCode code,
                        capsid::host::SafeReadErrorCode expected,
                        const char* label) {
    if (code != expected) {
        fail(std::string(label) + ": unexpected error code");
    }
}

}  // namespace

int main() {
    using capsid::host::SafeReadErrorCode;
    using capsid::host::safe_read_version_artifacts;
    using capsid::host::safe_read_regular_file;
    using capsid::host::VersionArtifacts;

    // 1. Legal source-only version.
    {
        Fixture fixture = make_fixture("orders", "v1");
        const std::string vdir = "orders/v1";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(),
                      "{\"app\":\"orders\"}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(),
                      "export default { fetch: () => new Response('ok') };");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v1",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require(result.code == SafeReadErrorCode::kNone,
                "legal source version rejected: " + result.message);
        require(!result.artifacts.has_bytecode,
                "source-only version reported bytecode");
        require(result.artifacts.capsid_json.identity.inode != 0 &&
                    result.artifacts.bundle.identity.size > 0,
                "identity not populated for legal files");
        require(result.artifacts.bundle.bytes.size() > 0,
                "bundle bytes empty");
        close(fixture.root_fd);
    }

    // 2. Legal trusted-bytecode version (all three files).
    {
        Fixture fixture = make_fixture("orders", "v2");
        const std::string vdir = "orders/v2";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
        write_file_at(fixture.root_fd, (vdir + "/bundle.qjsb").c_str(), "bc");
        write_file_at(fixture.root_fd, (vdir + "/bytecode.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bytecode.sig").c_str(), "sig");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v2",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require(result.code == SafeReadErrorCode::kNone,
                "legal trusted version rejected: " + result.message);
        require(result.artifacts.has_bytecode,
                "trusted version did not report bytecode");
        require(result.artifacts.bytecode.bytes.size() == 2 &&
                    result.artifacts.signature.bytes.size() == 3,
                "bytecode triple content wrong");
        close(fixture.root_fd);
    }

    // 3. Symlink in the tree rejects.
    {
        Fixture fixture = make_fixture("orders", "v3");
        const std::string vdir = "orders/v3";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
        require(symlinkat("bundle.mjs", fixture.root_fd,
                          (vdir + "/bundle.qjsb").c_str()) == 0,
                "cannot create symlink fixture");
        write_file_at(fixture.root_fd, (vdir + "/bytecode.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bytecode.sig").c_str(), "s");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v3",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require_error_code(result.code, SafeReadErrorCode::kNotRegularFile,
                           "symlink artifact");
        close(fixture.root_fd);
    }

    // 4. FIFO rejects without blocking (O_NONBLOCK).
    {
        Fixture fixture = make_fixture("orders", "v4");
        const std::string vdir = "orders/v4";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        require(mkfifoat(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), 0600) == 0,
                "cannot create FIFO fixture");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v4",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require_error_code(result.code, SafeReadErrorCode::kNotRegularFile,
                          "FIFO artifact");
        close(fixture.root_fd);
    }

    // 5. Socket rejects.
    {
        Fixture fixture = make_fixture("orders", "v5");
        const std::string vdir = "orders/v5";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        const std::string socket_path = fixture.root + "/" + vdir + "/bundle.mjs";
        const int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        require(socket_fd >= 0, "cannot create socket");
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(),
                     sizeof(address.sun_path) - 1);
        require(bind(socket_fd, reinterpret_cast<struct sockaddr*>(&address),
                     sizeof(address)) == 0,
                "cannot bind socket fixture");
        close(socket_fd);
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v5",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require_error_code(result.code, SafeReadErrorCode::kNotRegularFile,
                          "socket artifact");
        close(fixture.root_fd);
    }

    // 6. Directory rejects.
    {
        Fixture fixture = make_fixture("orders", "v6");
        const std::string vdir = "orders/v6";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        require(mkdirat(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), 0700) == 0,
                "cannot create directory fixture");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v6",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require_error_code(result.code, SafeReadErrorCode::kNotRegularFile,
                          "directory artifact");
        close(fixture.root_fd);
    }

    // 7. Over-limit file rejects.
    {
        Fixture fixture = make_fixture("orders", "v7");
        const std::string vdir = "orders/v7";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        const std::string huge(1024U * 1024U, 'a');
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), huge);
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v7",
                                        1024U);  // tiny total limit
        require_error_code(result.code, SafeReadErrorCode::kOverLimit,
                          "over-limit artifact");
        close(fixture.root_fd);
    }

    // 8. Hostile paths reject.
    {
        Fixture fixture = make_fixture("orders", "v8");
        const std::string vdir = "orders/v8";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
        for (const char* bad : { "orders/v8/../v8/capsid.json",
                                 "/orders/v8/capsid.json",
                                 "orders//v8/capsid.json",
                                 "orders/v8/./capsid.json" }) {
            const capsid::host::SafeReadResult result =
                safe_read_regular_file(fixture.root_fd, bad,
                                       capsid::host::kMaxArtifactFileBytes);
            require_error_code(result.code, SafeReadErrorCode::kInvalidPath,
                              "hostile path");
        }
        close(fixture.root_fd);
    }

    // 9. Partial bytecode triple rejects (all-or-none).
    {
        Fixture fixture = make_fixture("orders", "v9");
        const std::string vdir = "orders/v9";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
        write_file_at(fixture.root_fd, (vdir + "/bundle.qjsb").c_str(), "bc");
        // bytecode.json and bytecode.sig absent.
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v9",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require_error_code(result.code, SafeReadErrorCode::kMissingFile,
                          "partial bytecode triple");
        close(fixture.root_fd);
    }

    // 9b. Partial bytecode sets: any one or two files reject all-or-none.
    {
        struct PartialCase {
            bool qjsb;
            bool json;
            bool sig;
        };
        const PartialCase cases[] = {
            { true, false, false }, { false, true, false },
            { false, false, true }, { true, true, false },
            { true, false, true },  { false, true, true },
        };
        int index = 0;
        for (const PartialCase& partial : cases) {
            Fixture fixture = make_fixture("orders",
                                           ("v9p" + std::to_string(index)).c_str());
            const std::string vdir =
                std::string("orders/v9p") + std::to_string(index);
            write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
            write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
            if (partial.qjsb) {
                write_file_at(fixture.root_fd, (vdir + "/bundle.qjsb").c_str(), "bc");
            }
            if (partial.json) {
                write_file_at(fixture.root_fd, (vdir + "/bytecode.json").c_str(), "{}");
            }
            if (partial.sig) {
                write_file_at(fixture.root_fd, (vdir + "/bytecode.sig").c_str(), "sig");
            }
            const capsid::host::SafeReadResult result =
                safe_read_version_artifacts(fixture.root_fd, "orders",
                                            ("v9p" + std::to_string(index)).c_str(),
                                            capsid::host::kMaxVersionArtifactTotalBytes);
            require_error_code(result.code, SafeReadErrorCode::kMissingFile,
                              "partial bytecode set");
            close(fixture.root_fd);
            index += 1;
        }
    }

    // 9c. Formal App/Version ID grammar: separators and traversal never
    // become nested paths.
    {
        Fixture fixture = make_fixture("orders", "v9g");
        const std::string vdir = "orders/v9g";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), "x");
        for (const char* bad_app : { "foo/bar", "../orders", "", "a..b", ".hidden" }) {
            const capsid::host::SafeReadResult result =
                safe_read_version_artifacts(fixture.root_fd, bad_app, "v9g",
                                            capsid::host::kMaxVersionArtifactTotalBytes);
            require_error_code(result.code, SafeReadErrorCode::kInvalidPath,
                              "invalid app id");
        }
        for (const char* bad_version : { "v/1", "..", "", "v..1" }) {
            const capsid::host::SafeReadResult result =
                safe_read_version_artifacts(fixture.root_fd, "orders", bad_version,
                                            capsid::host::kMaxVersionArtifactTotalBytes);
            require_error_code(result.code, SafeReadErrorCode::kInvalidPath,
                              "invalid version id");
        }
        close(fixture.root_fd);
    }

    // 10. In-place replacement / mid-read truncation: a large file
    // truncated by a concurrent thread while the reader is reading must
    // fail as an identity change.
    {
        Fixture fixture = make_fixture("orders", "v10");
        const std::string vdir = "orders/v10";
        // Under the per-file limit (16 MiB) but large enough that the
        // truncation lands inside the read window.
        const std::string big(12U * 1024U * 1024U, 'b');
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(), "{}");
        write_file_at(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), big);
        std::thread truncator([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const std::string path = fixture.root + "/" + vdir + "/bundle.mjs";
            require(truncate(path.c_str(), 1024) == 0,
                    "cannot truncate fixture");
        });
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v10",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        truncator.join();
        require(result.code == SafeReadErrorCode::kIdentityChanged,
                "mid-read truncation was not detected (code " +
                    std::to_string(static_cast<int>(result.code)) + ")");
        close(fixture.root_fd);
    }

    // 11. Error messages never contain file content.
    {
        Fixture fixture = make_fixture("orders", "v11");
        const std::string vdir = "orders/v11";
        const std::string secret_content = "super-secret-canary-4711";
        write_file_at(fixture.root_fd, (vdir + "/capsid.json").c_str(),
                      "{\"canary\":\"" + secret_content + "\"}");
        require(mkfifoat(fixture.root_fd, (vdir + "/bundle.mjs").c_str(), 0600) == 0,
                "cannot create FIFO fixture");
        const capsid::host::SafeReadResult result =
            safe_read_version_artifacts(fixture.root_fd, "orders", "v11",
                                        capsid::host::kMaxVersionArtifactTotalBytes);
        require(result.message.find(secret_content) == std::string::npos,
                "error message leaked file content");
        close(fixture.root_fd);
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
