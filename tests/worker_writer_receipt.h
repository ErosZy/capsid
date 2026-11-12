#ifndef CAPSID_TESTS_WORKER_WRITER_RECEIPT_H
#define CAPSID_TESTS_WORKER_WRITER_RECEIPT_H

#include "win32_compat.h"
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <string>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace capsid_test {
inline std::string writer_receipt_path(int64_t pid) {
    return "capsid-test-writer-"+std::to_string(pid)+".receipt";
}

// Open one exclusive, owned-child receipt before the Worker enters its runtime.
// Its only content is emitted by the real write/ACK observer after both barriers.
class WriterReceipt {
public:
    WriterReceipt() {
#if defined(_WIN32)
        pid_ = static_cast<int64_t>(GetCurrentProcessId());
        handle_ = CreateFileA(writer_receipt_path(pid_).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
#else
        pid_ = static_cast<int64_t>(getpid());
        descriptor_ = open(writer_receipt_path(pid_).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    }
    ~WriterReceipt() {
#if defined(_WIN32)
        if (valid()) CloseHandle(handle_);
#else
        if (valid()) close(descriptor_);
#endif
    }
    WriterReceipt(const WriterReceipt&) = delete;
    WriterReceipt& operator=(const WriterReceipt&) = delete;
    bool valid() const {
#if defined(_WIN32)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }
    bool publish(uint64_t dial, size_t queued, uint64_t stalls, uint64_t blocked) {
        char buffer[256];
        const int length = std::snprintf(buffer, sizeof(buffer), "WRITER_PRESSURE %llu %llu %zu 4096 %llu %llu\n",
            static_cast<unsigned long long>(pid_), static_cast<unsigned long long>(dial), queued,
            static_cast<unsigned long long>(stalls), static_cast<unsigned long long>(blocked));
        if (!valid() || length <= 0 || static_cast<size_t>(length) >= sizeof(buffer)) return false;
#if defined(_WIN32)
        DWORD written = 0;
        return WriteFile(handle_, buffer, static_cast<DWORD>(length), &written, nullptr) && written == static_cast<DWORD>(length);
#else
        size_t offset = 0;
        while (offset < static_cast<size_t>(length)) {
            const ssize_t written = write(descriptor_, buffer+offset, static_cast<size_t>(length)-offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
            offset += static_cast<size_t>(written);
        }
        return true;
#endif
    }
private:
    int64_t pid_ = 0;
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};
}  // namespace capsid_test
#endif
