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

// Binding v1 §6: one LOAD_BINDING descriptor, decoded and validated when
// the binding's final chunk arrives. Source is the raw index.js bytes; the
// policy fields carry the effective (Manifest ∩ App) resource permissions.
struct WorkerBindingSecret {
    std::string key;
    std::vector<uint8_t> value;
};

struct WorkerBindingDescriptor {
    std::string name;
    std::vector<uint8_t> source;
    std::string config_json;
    std::vector<WorkerBindingSecret> secrets;
    std::vector<std::string> profiles;   // sandbox.requires (fixed set)
    std::vector<std::string> net_rules;  // effective allow targets
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    std::vector<std::string> env;
    std::vector<std::string> stdio;
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
    // Binding descriptors in arrival order; empty for zero-binding workers.
    const std::vector<WorkerBindingDescriptor> &bindings() const {
        return bindings_;
    }

private:
    bool consume_load_binding(const protocol::Frame &frame,
                              std::string *error);

    WorkerStartupConfig config_;
    bool hello_received_;
    bool bundle_started_;
    bool bundle_complete_;
    bool bundle_is_trusted_bytecode_;
    std::vector<uint8_t> bundle_;
    std::string bundle_name_;
    std::vector<WorkerBindingDescriptor> bindings_;
    // In-flight LOAD_BINDING blob accumulation (descriptor + source).
    std::vector<uint8_t> binding_blob_;
    bool binding_inflight_ = false;
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
