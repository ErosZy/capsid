#ifndef CAPSID_IPC_VALIDATION_H
#define CAPSID_IPC_VALIDATION_H

#include "capability_policy.h"
#include "protocol.h"

#include <stdint.h>

#include <string>
#include <vector>

namespace capsid {

struct WorkerStartupConfig {
    WorkerStartupConfig();

    uint32_t js_stack_size;
    uint32_t max_inflight;
    uint32_t initial_window;
    uint32_t max_header_bytes;
    uint32_t max_queued_bytes;
    uint32_t file_descriptor_limit;
    uint64_t timeout_ms;
    bool strict_sandbox;
    uint32_t sandbox_required_features;
    uint32_t preinstalled_sandbox_features;
    uint64_t js_heap_limit;
    uint64_t process_memory_limit;
    uint64_t max_fetch_request_body_bytes;
    uint64_t max_fetch_response_body_bytes;
    std::string tls_ca_bundle_path;
    bool legacy_egress_configured;
    EgressPolicy egress_policy;
    CapabilityPolicy capability_policy;
};

class WorkerStartupState {
public:
    WorkerStartupState();

    bool consume(const protocol::Frame &frame, std::string *error);

    bool hello_received() const { return hello_received_; }
    bool bundle_started() const { return bundle_started_; }
    bool bundle_complete() const { return bundle_complete_; }
    const WorkerStartupConfig &config() const { return config_; }
    WorkerStartupConfig &config() { return config_; }
    const std::vector<uint8_t> &bundle() const { return bundle_; }
    std::vector<uint8_t> &bundle() { return bundle_; }
    const std::string &bundle_name() const { return bundle_name_; }
    bool bundle_is_trusted_bytecode() const {
        return bundle_is_trusted_bytecode_;
    }

private:
    WorkerStartupConfig config_;
    bool hello_received_;
    bool bundle_started_;
    bool bundle_complete_;
    bool bundle_is_trusted_bytecode_;
    std::vector<uint8_t> bundle_;
    std::string bundle_name_;
};

struct WorkerRequestHeader {
    std::string name;
    std::string value;
};

struct WorkerRequestHead {
    std::string method;
    std::string url;
    std::vector<WorkerRequestHeader> headers;
    // True when the RequestHead carried kFlagRequestEnd: the request has no
    // body, so no request-direction credit is granted and the request
    // direction ends immediately.
    bool bodyless = false;
};

bool decode_worker_request_head(const protocol::Frame &frame,
                                uint32_t max_header_bytes,
                                WorkerRequestHead *output,
                                std::string *error);

}  // namespace capsid

#endif
