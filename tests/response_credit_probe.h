#pragma once

#include "capsid/runtime.h"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace capsid_test {
// Shared by the C ABI observation consumers. The optional small-window mode
// exposes complete wire-body pieces for regression validation; ordinary runs
// keep their existing output. This helper queues credit and never flushes IPC.
class ResponseCreditProbe {
public:
    explicit ResponseCreditProbe(uint32_t window = 0, uint64_t group = 0)
        : window_(window), group_(group) {}

    void consume(capsid_worker* worker, const capsid_event& event) {
        if (event.type != CAPSID_EVENT_RESPONSE_BODY || !event.request_id ||
            event.payload.size > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("response credit body event");
        if (request_ != event.request_id) {
            request_ = event.request_id;
            consumed_ = granted_ = 0;
            parts_.clear();
        }
        const auto bytes = static_cast<uint32_t>(event.payload.size);
        if (!bytes) return;
        if (window_ && bytes > window_)
            throw std::runtime_error("response body exceeded configured test window");
        if (capsid_worker_grant_response_credit(worker, event.request_id, bytes) != CAPSID_OK)
            throw std::runtime_error("grant consumed response credit");
        const uint32_t granted = bytes;
        consumed_ += bytes;
        granted_ += granted;
        parts_.push_back(bytes);
        if (window_) {
            std::printf("RESPONSE_WINDOW_PART {\"window\":%u,\"group\":%llu,\"request\":%llu,\"bytes\":%u,\"granted\":%u,\"bodyHex\":\"",
                window_, static_cast<unsigned long long>(group_),
                static_cast<unsigned long long>(request_), bytes, granted);
            hex(event.payload.data, event.payload.size);
            std::puts("\"}");
            std::fflush(stdout);
        }
    }

    void complete(uint64_t request, const std::string& body) const {
        if (!window_) return;
        if (request != request_ || consumed_ != body.size())
            throw std::runtime_error("complete consumed response accounting");
        std::printf("RESPONSE_WINDOW_ROW {\"window\":%u,\"group\":%llu,\"request\":%llu,\"bytes\":%llu,\"granted\":%llu,\"parts\":[",
            window_, static_cast<unsigned long long>(group_),
            static_cast<unsigned long long>(request_),
            static_cast<unsigned long long>(consumed_), static_cast<unsigned long long>(granted_));
        for (size_t i = 0; i < parts_.size(); ++i)
            std::printf("%s%u", i ? "," : "", parts_[i]);
        std::printf("],\"bodyHex\":\"");
        hex(reinterpret_cast<const uint8_t*>(body.data()), body.size());
        std::puts("\"}");
        std::fflush(stdout);
    }

private:
    static void hex(const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) std::printf("%02x", unsigned(data[i]));
    }
    uint32_t window_;
    uint64_t group_;
    uint64_t request_ = 0, consumed_ = 0, granted_ = 0;
    std::vector<uint32_t> parts_;
};
} // namespace capsid_test
