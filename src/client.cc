#include "capsid/runtime.h"

#include "capability_policy.h"
#include "client_ipc_metrics.h"
#include "cpu_topology.h"
#include "egress_policy.h"
#include "protocol.h"
#include "response_headers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef CAPSID_WORKER_DEFAULT_PATH
#define CAPSID_WORKER_DEFAULT_PATH "capsid-worker"
#endif

struct capsid_worker {
    struct RequestState {
        uint64_t credit;
        bool ended;
        std::chrono::steady_clock::time_point deadline;

        RequestState() : credit(0), ended(false) {}
    };

    int fd;
    pid_t pid;
    bool closed;
    uint64_t request_timeout_ms;
    uint32_t max_inflight_requests;
    uint32_t max_header_bytes;
    uint32_t max_queued_bytes;
    size_t write_offset;
    std::vector<uint8_t> write_buffer;
    capsid::protocol::Parser parser;
    std::vector<uint8_t> event_payload;
    std::map<uint64_t, RequestState> requests;
    std::set<uint64_t> canceled_requests;
    std::deque<uint64_t> canceled_request_order;
    bool ipc_metrics_enabled;
    capsid::ClientIpcMetrics ipc_metrics;
};

namespace {

const uint64_t kWorkerTimeoutGraceMs = 250;
const uint32_t kCgroupResourceLimitFields =
    CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
    CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT |
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH |
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX |
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX |
    CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX;

uint64_t default_u64(uint64_t value, uint64_t fallback) {
    return value ? value : fallback;
}

uint32_t default_u32(uint32_t value, uint32_t fallback) {
    return value ? value : fallback;
}

void forget_canceled_request(capsid_worker *worker, uint64_t request_id) {
    worker->canceled_requests.erase(request_id);
    worker->canceled_request_order.erase(
        std::remove(
            worker->canceled_request_order.begin(),
            worker->canceled_request_order.end(),
            request_id),
        worker->canceled_request_order.end());
}

void remember_canceled_request(capsid_worker *worker, uint64_t request_id) {
    if (!worker->canceled_requests.insert(request_id).second) {
        return;
    }
    worker->canceled_request_order.push_back(request_id);
    const size_t configured_limit =
        static_cast<size_t>(worker->max_inflight_requests);
    const size_t doubled_limit =
        configured_limit > std::numeric_limits<size_t>::max() / 2
            ? std::numeric_limits<size_t>::max()
            : configured_limit * 2;
    const size_t limit = std::max<size_t>(
        64,
        doubled_limit);
    while (worker->canceled_requests.size() > limit &&
           !worker->canceled_request_order.empty()) {
        const uint64_t oldest = worker->canceled_request_order.front();
        worker->canceled_request_order.pop_front();
        worker->canceled_requests.erase(oldest);
    }
}

bool is_canceled_request_frame(const capsid::protocol::Frame &frame) {
    switch (frame.type) {
        case capsid::protocol::kWindowUpdate:
        case capsid::protocol::kResponseHead:
        case capsid::protocol::kResponseBody:
        case capsid::protocol::kResponseEnd:
        case capsid::protocol::kError:
            return frame.request_id != 0;
        default:
            return false;
    }
}

bool set_nonblocking_cloexec(int fd) {
    const int status_flags = fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    return status_flags >= 0 && descriptor_flags >= 0 &&
           fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
}

ssize_t write_socket(int fd, const uint8_t *data, size_t size) {
#ifdef MSG_NOSIGNAL
    return send(fd, data, size, MSG_NOSIGNAL);
#else
    return send(fd, data, size, 0);
#endif
}

capsid_result queue_wire(capsid_worker *worker, const std::vector<uint8_t> &wire) {
    if (!worker || worker->closed) {
        return CAPSID_CLOSED;
    }
    const size_t queued = worker->write_buffer.size() - worker->write_offset;
    if (wire.size() > worker->max_queued_bytes ||
        queued > worker->max_queued_bytes - wire.size()) {
        if (worker->ipc_metrics_enabled) {
            ++worker->ipc_metrics.queue_would_block;
        }
        return CAPSID_WOULD_BLOCK;
    }
    if (worker->write_offset && worker->write_offset == worker->write_buffer.size()) {
        worker->write_buffer.clear();
        worker->write_offset = 0;
    }
    worker->write_buffer.insert(worker->write_buffer.end(), wire.begin(), wire.end());
    if (worker->ipc_metrics_enabled) {
        worker->ipc_metrics.queued_wire_bytes += wire.size();
        const size_t outstanding =
            worker->write_buffer.size() - worker->write_offset;
        worker->ipc_metrics.queued_bytes_high_water =
            std::max(
                worker->ipc_metrics.queued_bytes_high_water,
                outstanding);
    }
    return CAPSID_OK;
}

capsid_result queue_frame(capsid_worker *worker, const capsid::protocol::Frame &frame) {
    std::vector<uint8_t> wire;
    if (!capsid::protocol::encode(frame, &wire)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const capsid_result result = queue_wire(worker, wire);
    if (result == CAPSID_OK && worker->ipc_metrics_enabled) {
        ++worker->ipc_metrics.queued_frames;
    }
    return result;
}

capsid_result queue_chunked(capsid_worker *worker,
                          uint16_t type,
                          uint64_t request_id,
                          const uint8_t *data,
                          size_t size,
                          bool mark_start,
                          bool mark_end,
                          uint32_t first_flags = 0) {
    if (size && !data) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (!worker || worker->closed) {
        return CAPSID_CLOSED;
    }
    const size_t frame_count =
        size == 0
            ? 1
            : size / capsid::protocol::kMaxPayloadSize +
                  (size % capsid::protocol::kMaxPayloadSize != 0 ? 1 : 0);
    if (frame_count >
        (std::numeric_limits<size_t>::max() - size) /
            capsid::protocol::kHeaderSize) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const size_t wire_size =
        size + frame_count * capsid::protocol::kHeaderSize;
    const size_t queued =
        worker->write_buffer.size() - worker->write_offset;
    if (wire_size > worker->max_queued_bytes ||
        queued > worker->max_queued_bytes - wire_size) {
        if (worker->ipc_metrics_enabled) {
            ++worker->ipc_metrics.queue_would_block;
        }
        return CAPSID_WOULD_BLOCK;
    }

    std::vector<uint8_t> batch;
    batch.reserve(wire_size);
    size_t offset = 0;
    bool first = true;
    do {
        const size_t chunk_size =
            std::min(size - offset, static_cast<size_t>(capsid::protocol::kMaxPayloadSize));
        capsid::protocol::Frame frame;
        frame.type = type;
        frame.request_id = request_id;
        frame.flags = first ? first_flags : 0;
        if (first && mark_start) {
            frame.flags |= capsid::protocol::kFlagStart;
        }
        if (offset + chunk_size == size && mark_end) {
            frame.flags |= capsid::protocol::kFlagEnd;
        }
        if (chunk_size) {
            frame.payload.assign(data + offset, data + offset + chunk_size);
        }
        std::vector<uint8_t> wire;
        if (!capsid::protocol::encode(frame, &wire)) {
            return CAPSID_INVALID_ARGUMENT;
        }
        batch.insert(batch.end(), wire.begin(), wire.end());
        offset += chunk_size;
        first = false;
    } while (offset < size || first);
    const capsid_result result = queue_wire(worker, batch);
    if (result == CAPSID_OK && worker->ipc_metrics_enabled) {
        worker->ipc_metrics.queued_frames += frame_count;
    }
    return result;
}

void append_string16(std::vector<uint8_t> *output, const uint8_t *data, size_t size) {
    capsid::protocol::append_u16(output, static_cast<uint16_t>(size));
    if (size != 0) {
        output->insert(output->end(), data, data + size);
    }
}

void append_string32(std::vector<uint8_t> *output, const uint8_t *data, size_t size) {
    capsid::protocol::append_u32(output, static_cast<uint32_t>(size));
    if (size != 0) {
        output->insert(output->end(), data, data + size);
    }
}

capsid_result send_hello(capsid_worker *worker,
                       const capsid_worker_config &config,
                       const capsid::CapabilityPolicy
                           &validated_capability,
                       uint32_t file_descriptor_limit,
                       uint32_t preinstalled_sandbox_features) {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kHello;
    frame.flags = 0;
    frame.request_id = 0;
    capsid::protocol::append_u32(&frame.payload, CAPSID_ABI_VERSION);
    capsid::protocol::append_u64(&frame.payload, config.js_heap_limit);
    capsid::protocol::append_u64(&frame.payload, config.process_memory_limit);
    capsid::protocol::append_u32(
        &frame.payload, file_descriptor_limit);
    capsid::protocol::append_u64(&frame.payload, config.request_timeout_ms);
    capsid::protocol::append_u32(&frame.payload, config.js_stack_size);
    capsid::protocol::append_u32(&frame.payload, config.max_inflight_requests);
    capsid::protocol::append_u32(&frame.payload, config.initial_stream_window);
    capsid::protocol::append_u32(&frame.payload, config.max_header_bytes);
    capsid::protocol::append_u32(&frame.payload, config.max_queued_bytes);
    frame.payload.push_back(config.strict_sandbox);
    capsid::protocol::append_u32(
        &frame.payload, config.sandbox_required_features);
    capsid::protocol::append_u32(
        &frame.payload, preinstalled_sandbox_features);
    const char *ca_bundle =
        config.tls_ca_bundle_path ? config.tls_ca_bundle_path : "";
    append_string16(
        &frame.payload,
        reinterpret_cast<const uint8_t *>(ca_bundle),
        std::strlen(ca_bundle));
    capsid::protocol::append_u64(
        &frame.payload, config.max_fetch_request_body_bytes);
    capsid::protocol::append_u64(
        &frame.payload, config.max_fetch_response_body_bytes);
    const capsid_egress_policy *policy = config.egress_policy;
    capsid::protocol::append_u32(
        &frame.payload,
        static_cast<uint32_t>(
            policy ? policy->default_action : CAPSID_EGRESS_DENY));
    capsid::protocol::append_u32(
        &frame.payload, policy ? policy->rule_count : 0);
    if (policy) {
        for (uint32_t index = 0; index < policy->rule_count; ++index) {
            const capsid_egress_rule &rule = policy->rules[index];
            capsid::protocol::append_u32(
                &frame.payload, static_cast<uint32_t>(rule.action));
            capsid::protocol::append_u16(
                &frame.payload, rule.port_start);
            capsid::protocol::append_u16(
                &frame.payload, rule.port_end);
            capsid::protocol::append_u32(
                &frame.payload, rule.rule_id);
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(rule.target),
                std::strlen(rule.target));
        }
    }
    frame.payload.push_back(policy ? 1 : 0);
    const capsid_capability_policy *capability =
        config.capability_policy;
    capsid::protocol::append_u32(
        &frame.payload,
        capability ? capability->version : 0);
    const char *application_identity =
        capability && capability->application_identity
            ? capability->application_identity
            : "";
    append_string16(
        &frame.payload,
        reinterpret_cast<const uint8_t *>(application_identity),
        std::strlen(application_identity));
    capsid::protocol::append_u16(
        &frame.payload,
        static_cast<uint16_t>(
            capability ? capability->allowed_module_count : 0));
    if (capability) {
        for (uint32_t index = 0;
             index < capability->allowed_module_count;
             ++index) {
            const char *module =
                capability->allowed_modules[index];
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(module),
                std::strlen(module));
        }
    }
    capsid::protocol::append_u16(
        &frame.payload,
        static_cast<uint16_t>(
            capability ? capability->rule_count : 0));
    if (capability) {
        for (uint32_t index = 0;
             index < capability->rule_count;
             ++index) {
            const capsid_permission_rule &rule =
                capability->rules[index];
            capsid::protocol::append_u32(
                &frame.payload,
                static_cast<uint32_t>(rule.action));
            capsid::protocol::append_u32(
                &frame.payload,
                static_cast<uint32_t>(rule.permission));
            capsid::protocol::append_u32(
                &frame.payload, rule.rule_id);
            const char *resource =
                rule.resource ? rule.resource : "";
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(resource),
                std::strlen(resource));
        }
    }
    const capsid_egress_policy *capability_net =
        capability ? capability->net_policy : NULL;
    capsid::protocol::append_u32(
        &frame.payload,
        static_cast<uint32_t>(
            capability_net
                ? capability_net->default_action
                : CAPSID_EGRESS_DENY));
    capsid::protocol::append_u32(
        &frame.payload,
        capability_net ? capability_net->rule_count : 0);
    if (capability_net) {
        for (uint32_t index = 0;
             index < capability_net->rule_count;
             ++index) {
            const capsid_egress_rule &rule =
                capability_net->rules[index];
            capsid::protocol::append_u32(
                &frame.payload,
                static_cast<uint32_t>(rule.action));
            capsid::protocol::append_u16(
                &frame.payload, rule.port_start);
            capsid::protocol::append_u16(
                &frame.payload, rule.port_end);
            capsid::protocol::append_u32(
                &frame.payload, rule.rule_id);
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(rule.target),
                std::strlen(rule.target));
        }
    }
    const std::vector<
        std::pair<std::string, std::string> > &environment =
            validated_capability.env_entries();
    capsid::protocol::append_u16(
        &frame.payload,
        static_cast<uint16_t>(environment.size()));
    for (std::vector<
             std::pair<std::string, std::string> >::const_iterator
             it = environment.begin();
         it != environment.end();
         ++it) {
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(
                it->first.data()),
            it->first.size());
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(
                it->second.data()),
            it->second.size());
    }
    return queue_frame(worker, frame);
}

#if defined(__linux__)
struct CgroupSetting {
    std::string name;
    std::string desired;
    std::string original;
    bool applied;

    CgroupSetting(const char *setting_name,
                  const std::string &setting_value)
        : name(setting_name),
          desired(setting_value),
          applied(false) {}
};

bool cgroup_read_at(int directory,
                    const char *name,
                    std::string *value) {
    const int descriptor =
        openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    char buffer[256];
    size_t size = 0;
    while (size < sizeof(buffer)) {
        const ssize_t count =
            read(descriptor, buffer + size, sizeof(buffer) - size);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(descriptor);
            return false;
        }
        if (count == 0) {
            break;
        }
        size += static_cast<size_t>(count);
    }
    close(descriptor);
    if (size == sizeof(buffer)) {
        errno = EOVERFLOW;
        return false;
    }
    size_t begin = 0;
    while (begin < size &&
           (buffer[begin] == ' ' || buffer[begin] == '\t' ||
            buffer[begin] == '\r' || buffer[begin] == '\n')) {
        ++begin;
    }
    while (size > begin &&
           (buffer[size - 1] == ' ' || buffer[size - 1] == '\t' ||
            buffer[size - 1] == '\r' || buffer[size - 1] == '\n')) {
        --size;
    }
    value->assign(buffer + begin, buffer + size);
    return true;
}

bool cgroup_write_at(int directory,
                     const char *name,
                     const std::string &value) {
    const int descriptor =
        openat(directory, name, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    ssize_t count;
    do {
        count = write(descriptor, value.data(), value.size());
    } while (count < 0 && errno == EINTR);
    const int write_error = errno;
    close(descriptor);
    if (count != static_cast<ssize_t>(value.size())) {
        errno = count < 0 ? write_error : EIO;
        return false;
    }
    return true;
}

std::string cgroup_limit_value(uint64_t value) {
    return value == CAPSID_RESOURCE_UNLIMITED
               ? std::string("max")
               : std::to_string(value);
}

bool cgroup_pid_list_contains(const std::string &members,
                              const std::string &pid) {
    size_t begin = 0;
    while (begin <= members.size()) {
        const size_t end = members.find('\n', begin);
        const size_t length =
            end == std::string::npos ? members.size() - begin
                                     : end - begin;
        if (length == pid.size() &&
            members.compare(begin, length, pid) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return false;
}

void rollback_cgroup_settings(
    int directory,
    std::vector<CgroupSetting> *settings) {
    for (std::vector<CgroupSetting>::reverse_iterator it =
             settings->rbegin();
         it != settings->rend();
         ++it) {
        if (it->applied) {
            cgroup_write_at(directory, it->name.c_str(), it->original);
        }
    }
}

bool configure_and_attach_cgroup_v2(
    pid_t pid,
    const char *path,
    const capsid_resource_limits &limits) {
    const int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return false;
    }
    struct statfs filesystem;
    if (fstatfs(directory, &filesystem) != 0 ||
        static_cast<unsigned long>(filesystem.f_type) !=
            static_cast<unsigned long>(CGROUP2_SUPER_MAGIC)) {
        close(directory);
        errno = ENOTSUP;
        return false;
    }

    std::vector<CgroupSetting> settings;
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX) != 0) {
        settings.push_back(CgroupSetting(
            "cpu.max",
            cgroup_limit_value(limits.cgroup_cpu_quota_us) + " " +
                std::to_string(limits.cgroup_cpu_period_us)));
    }
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT) != 0) {
        settings.push_back(CgroupSetting(
            "cpu.weight",
            std::to_string(limits.cgroup_cpu_weight)));
    }
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH) != 0) {
        settings.push_back(CgroupSetting(
            "memory.high",
            cgroup_limit_value(limits.cgroup_memory_high_bytes)));
    }
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX) != 0) {
        settings.push_back(CgroupSetting(
            "memory.max",
            cgroup_limit_value(limits.cgroup_memory_max_bytes)));
    }
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX) != 0) {
        settings.push_back(CgroupSetting(
            "memory.swap.max",
            cgroup_limit_value(
                limits.cgroup_memory_swap_max_bytes)));
    }
    if ((limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX) != 0) {
        settings.push_back(CgroupSetting(
            "pids.max",
            limits.cgroup_pids_max == CAPSID_RESOURCE_PIDS_UNLIMITED
                ? std::string("max")
                : std::to_string(limits.cgroup_pids_max)));
    }

    for (std::vector<CgroupSetting>::iterator it = settings.begin();
         it != settings.end();
         ++it) {
        if (!cgroup_read_at(
                directory, it->name.c_str(), &it->original)) {
            close(directory);
            return false;
        }
    }
    for (std::vector<CgroupSetting>::iterator it = settings.begin();
         it != settings.end();
         ++it) {
        /*
         * Mark it before the write so even an unexpected short controller
         * write is restored from the value captured above.
         */
        it->applied = true;
        if (!cgroup_write_at(
                directory, it->name.c_str(), it->desired)) {
            const int failure = errno;
            rollback_cgroup_settings(directory, &settings);
            close(directory);
            errno = failure;
            return false;
        }
        std::string actual;
        const bool read_back = cgroup_read_at(
            directory, it->name.c_str(), &actual);
        if (!read_back || actual != it->desired) {
            const int failure = read_back ? EIO : errno;
            rollback_cgroup_settings(directory, &settings);
            close(directory);
            errno = failure;
            return false;
        }
    }

    const std::string value = std::to_string(pid);
    if (!cgroup_write_at(directory, "cgroup.procs", value)) {
        const int failure = errno;
        rollback_cgroup_settings(directory, &settings);
        close(directory);
        errno = failure;
        return false;
    }
    std::string members;
    const bool membership_read =
        cgroup_read_at(directory, "cgroup.procs", &members);
    if (!membership_read ||
        !cgroup_pid_list_contains(members, value)) {
        const int failure = membership_read ? EIO : errno;
        rollback_cgroup_settings(directory, &settings);
        close(directory);
        errno = failure;
        return false;
    }
    close(directory);
    return true;
}
#endif

bool decode_header_at(const capsid_event *event,
                      size_t wanted_index,
                      size_t *out_count,
                      capsid_header *out_header);

capsid_result map_frame_to_event(capsid_worker *worker,
                               const capsid::protocol::Frame &frame,
                               capsid_event *event) {
    event->type = CAPSID_EVENT_NONE;
    event->request_id = frame.request_id;
    event->flags = frame.flags;
    event->status = 0;
    event->credit = 0;

    event->payload.data = worker->event_payload.empty() ? NULL : &worker->event_payload[0];
    event->payload.size = worker->event_payload.size();

    switch (frame.type) {
        case capsid::protocol::kReady:
            event->type = CAPSID_EVENT_READY;
            break;
        case capsid::protocol::kWindowUpdate: {
            if (event->payload.size != sizeof(uint32_t)) {
                return CAPSID_PROTOCOL_ERROR;
            }
            const uint8_t *cursor = event->payload.data;
            const uint8_t *end = cursor + event->payload.size;
            if (!capsid::protocol::read_u32(&cursor, end, &event->credit) || cursor != end) {
                return CAPSID_PROTOCOL_ERROR;
            }
            std::map<uint64_t, capsid_worker::RequestState>::iterator state =
                worker->requests.find(frame.request_id);
            if (state == worker->requests.end() ||
                state->second.credit >
                    std::numeric_limits<uint64_t>::max() - event->credit) {
                return CAPSID_PROTOCOL_ERROR;
            }
            state->second.credit += event->credit;
            event->type = CAPSID_EVENT_REQUEST_CREDIT;
            break;
        }
        case capsid::protocol::kResponseHead: {
            if (event->payload.size < sizeof(uint16_t)) {
                return CAPSID_PROTOCOL_ERROR;
            }
            const uint8_t *cursor = event->payload.data;
            const uint8_t *end = cursor + event->payload.size;
            uint16_t status = 0;
            if (!capsid::protocol::read_u16(&cursor, end, &status)) {
                return CAPSID_PROTOCOL_ERROR;
            }
            event->status = status;
            event->payload.data = cursor;
            event->payload.size = static_cast<size_t>(end - cursor);
            event->type = CAPSID_EVENT_RESPONSE_HEAD;
            if (!decode_header_at(event, 0, NULL, NULL)) {
                return CAPSID_PROTOCOL_ERROR;
            }
            break;
        }
        case capsid::protocol::kResponseBody:
            event->type = CAPSID_EVENT_RESPONSE_BODY;
            break;
        case capsid::protocol::kResponseEnd:
            event->type = CAPSID_EVENT_RESPONSE_END;
            worker->requests.erase(frame.request_id);
            break;
        case capsid::protocol::kLog:
            event->type = CAPSID_EVENT_LOG;
            break;
        case capsid::protocol::kAudit:
            event->type = CAPSID_EVENT_AUDIT;
            break;
        case capsid::protocol::kMemoryMetricsResponse:
            if (frame.request_id != 0) {
                return CAPSID_PROTOCOL_ERROR;
            }
            event->type = CAPSID_EVENT_MEMORY_METRICS;
            break;
        case capsid::protocol::kError:
            event->type =
                (frame.flags & capsid::protocol::kErrorFlagTimeout) != 0
                    ? CAPSID_EVENT_REQUEST_TIMEOUT
                    : CAPSID_EVENT_ERROR;
            if (frame.request_id != 0) {
                worker->requests.erase(frame.request_id);
            }
            break;
        default:
            return CAPSID_PROTOCOL_ERROR;
    }
    return CAPSID_OK;
}

bool wait_for_child(pid_t pid, uint32_t timeout_ms) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            return true;
        }
        if (result < 0 && errno != EINTR) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool decode_header_at(const capsid_event *event,
                      size_t wanted_index,
                      size_t *out_count,
                      capsid_header *out_header) {
    if (!event || event->type != CAPSID_EVENT_RESPONSE_HEAD ||
        !event->payload.data ||
        event->payload.size < sizeof(uint16_t) * 2) {
        return false;
    }
    const uint8_t *headers = event->payload.data;
    const uint8_t *end = headers + event->payload.size;
    uint16_t status_text_size = 0;
    if (!capsid::protocol::read_u16(
            &headers, end, &status_text_size) ||
        static_cast<size_t>(end - headers) < status_text_size) {
        return false;
    }
    headers += status_text_size;
    capsid::ResponseHeaderView decoded;
    if (!capsid::decode_response_headers(
            headers,
            static_cast<size_t>(end - headers),
            wanted_index,
            out_count,
            out_header ? &decoded : NULL)) {
        return false;
    }
    if (out_header) {
        out_header->name.data = decoded.name;
        out_header->name.size = decoded.name_size;
        out_header->value.data = decoded.value;
        out_header->value.size = decoded.value_size;
    }
    return true;
}

}  // namespace

namespace capsid {

ClientIpcMetrics::ClientIpcMetrics()
    : queued_frames(0),
      queued_wire_bytes(0),
      queue_would_block(0),
      flush_calls(0),
      socket_write_calls(0),
      socket_write_bytes(0),
      socket_write_eagain(0),
      next_event_calls(0),
      parsed_frames(0),
      parser_payload_copied_bytes(0),
      socket_read_calls(0),
      socket_read_bytes(0),
      socket_read_eagain(0),
      queued_bytes_high_water(0) {}

void client_ipc_metrics_enable(capsid_worker *worker, bool enabled) {
    if (!worker) {
        return;
    }
    worker->ipc_metrics_enabled = enabled;
    worker->ipc_metrics = ClientIpcMetrics();
}

void client_ipc_metrics_reset(capsid_worker *worker) {
    if (!worker) {
        return;
    }
    worker->ipc_metrics = ClientIpcMetrics();
}

bool client_ipc_metrics_snapshot(const capsid_worker *worker,
                                 ClientIpcMetrics *metrics) {
    if (!worker || !metrics || !worker->ipc_metrics_enabled) {
        return false;
    }
    *metrics = worker->ipc_metrics;
    return true;
}

}  // namespace capsid

extern "C" {

void capsid_resource_limits_init(capsid_resource_limits *limits) {
    if (!limits) {
        return;
    }
    std::memset(limits, 0, sizeof(*limits));
    limits->struct_size = sizeof(*limits);
}

void capsid_worker_config_init(capsid_worker_config *config) {
    if (!config) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = CAPSID_ABI_VERSION;
    config->js_heap_limit = 64u * 1024u * 1024u;
#if defined(__APPLE__) || defined(CAPSID_ASAN_BUILD)
    // macOS does not support the Linux address-space gate. ASan also reserves
    // a large shadow range that is incompatible with the production default;
    // its instrumented builds still enforce js_heap_limit inside QuickJS.
    config->process_memory_limit = 0;
#else
    config->process_memory_limit = 256u * 1024u * 1024u;
#endif
    config->request_timeout_ms = 30000;
    config->js_stack_size = 1024u * 1024u;
    config->max_inflight_requests = 128;
    config->max_header_bytes = 64u * 1024u;
    config->max_queued_bytes = 4u * 1024u * 1024u;
    config->initial_stream_window = 256u * 1024u;
    config->strict_sandbox = 0;
    config->tls_ca_bundle_path = NULL;
    config->max_fetch_request_body_bytes = 0;
    config->max_fetch_response_body_bytes = 0;
    config->sandbox_required_features = 0;
    config->sandbox_reserved = 0;
    config->sandbox_cgroup_path = NULL;
    config->resource_limits = NULL;
    config->egress_policy = NULL;
    config->capability_policy = NULL;
    config->sandbox_network_namespace_fd = -1;
    config->egress_reserved = 0;
}

const char *capsid_result_string(capsid_result result) {
    switch (result) {
        case CAPSID_OK:
            return "ok";
        case CAPSID_WOULD_BLOCK:
            return "operation would block";
        case CAPSID_CLOSED:
            return "worker is closed";
        case CAPSID_INVALID_ARGUMENT:
            return "invalid argument";
        case CAPSID_PROTOCOL_ERROR:
            return "protocol error";
        case CAPSID_SYSTEM_ERROR:
            return "system error";
        case CAPSID_CHILD_ERROR:
            return "worker process error";
    }
    return "unknown error";
}

capsid_result capsid_worker_spawn(const capsid_worker_config *input, capsid_worker **out_worker) {
    if (!input || !out_worker || input->struct_size < sizeof(capsid_worker_config) ||
        input->abi_version != CAPSID_ABI_VERSION) {
        return CAPSID_INVALID_ARGUMENT;
    }

    capsid_worker_config config = *input;
    capsid::EgressPolicy validated_egress_policy;
    std::string egress_policy_error;
    if (!validated_egress_policy.configure(
            config.egress_policy, &egress_policy_error)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::CapabilityPolicy validated_capability_policy;
    std::string capability_policy_error;
    if (!validated_capability_policy.configure(
            config.capability_policy,
            &capability_policy_error)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    size_t egress_rule_payload_size = 0;
    if (config.egress_policy) {
        for (uint32_t index = 0;
             index < config.egress_policy->rule_count;
             ++index) {
            egress_rule_payload_size +=
                14 + std::strlen(
                         config.egress_policy->rules[index].target);
        }
    }
    size_t capability_payload_size = 0;
    if (config.capability_policy) {
        const capsid_capability_policy &capability =
            *config.capability_policy;
        capability_payload_size +=
            std::strlen(
                capability.application_identity
                    ? capability.application_identity
                    : "");
        for (uint32_t index = 0;
             index < capability.allowed_module_count;
             ++index) {
            capability_payload_size +=
                2 + std::strlen(
                        capability.allowed_modules[index]);
        }
        for (uint32_t index = 0;
             index < capability.rule_count;
             ++index) {
            capability_payload_size +=
                14 + std::strlen(
                         capability.rules[index].resource
                             ? capability.rules[index].resource
                             : "");
        }
        if (capability.net_policy) {
            for (uint32_t index = 0;
                 index < capability.net_policy->rule_count;
                 ++index) {
                capability_payload_size +=
                    14 + std::strlen(
                             capability.net_policy
                                 ->rules[index]
                                 .target);
            }
        }
    }
    const std::vector<
        std::pair<std::string, std::string> >
        &validated_environment =
            validated_capability_policy.env_entries();
    capability_payload_size += 2;
    for (std::vector<
             std::pair<std::string, std::string> >::const_iterator
             it = validated_environment.begin();
         it != validated_environment.end();
         ++it) {
        capability_payload_size +=
            4 + it->first.size() + it->second.size();
    }
    capsid_resource_limits resource_limits;
    std::memset(&resource_limits, 0, sizeof(resource_limits));
    resource_limits.struct_size = sizeof(resource_limits);
    if (config.resource_limits) {
        if (config.resource_limits->struct_size !=
                sizeof(capsid_resource_limits)) {
            return CAPSID_INVALID_ARGUMENT;
        }
        resource_limits = *config.resource_limits;
    }
    uint32_t file_descriptor_limit = 64;
    if ((resource_limits.enabled_fields &
         CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS) != 0) {
        file_descriptor_limit = resource_limits.file_descriptors;
    }
    config.js_heap_limit = default_u64(config.js_heap_limit, 64u * 1024u * 1024u);
    config.request_timeout_ms = default_u64(config.request_timeout_ms, 30000);
    config.js_stack_size = default_u32(config.js_stack_size, 1024u * 1024u);
    config.max_inflight_requests = default_u32(config.max_inflight_requests, 128);
    config.max_header_bytes = default_u32(config.max_header_bytes, 64u * 1024u);
    config.max_queued_bytes = default_u32(config.max_queued_bytes, 4u * 1024u * 1024u);
    config.initial_stream_window = default_u32(config.initial_stream_window, 256u * 1024u);
    const size_t ca_bundle_path_size =
        config.tls_ca_bundle_path ? std::strlen(config.tls_ca_bundle_path) : 0;
    const size_t cgroup_path_size =
        config.sandbox_cgroup_path
            ? std::strlen(config.sandbox_cgroup_path)
            : 0;
    const uint64_t max_safe_js_integer = UINT64_C(9007199254740991);
    if (config.js_heap_limit >
            static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        (config.process_memory_limit != 0 &&
         config.process_memory_limit < config.js_heap_limit) ||
        config.request_timeout_ms >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        config.max_header_bytes < 8 ||
        config.max_header_bytes > capsid::protocol::kMaxPayloadSize ||
        config.max_queued_bytes <
            capsid::protocol::kHeaderSize +
                capsid::protocol::kHelloFixedPayloadSize ||
        config.max_queued_bytes > capsid::protocol::kMaxBufferedBytes ||
        ca_bundle_path_size > 4096 ||
        capsid::protocol::kHelloFixedPayloadSize +
                ca_bundle_path_size +
                egress_rule_payload_size +
                capability_payload_size >
            capsid::protocol::kMaxPayloadSize ||
        capsid::protocol::kHeaderSize +
                capsid::protocol::kHelloFixedPayloadSize +
                ca_bundle_path_size +
                egress_rule_payload_size +
                capability_payload_size >
            config.max_queued_bytes ||
        config.max_fetch_request_body_bytes > max_safe_js_integer ||
        config.max_fetch_response_body_bytes > max_safe_js_integer ||
        config.strict_sandbox > 1 ||
        config.sandbox_reserved != 0 ||
        config.egress_reserved != 0 ||
        config.sandbox_network_namespace_fd < -1 ||
        (config.sandbox_required_features &
         ~static_cast<uint32_t>(CAPSID_SANDBOX_FEATURE_ALL)) != 0 ||
        (!config.strict_sandbox &&
         config.sandbox_required_features != 0) ||
        cgroup_path_size > 4096 ||
        (cgroup_path_size != 0 && config.sandbox_cgroup_path[0] != '/') ||
        resource_limits.reserved != 0 ||
        (resource_limits.enabled_fields &
         ~static_cast<uint32_t>(CAPSID_RESOURCE_LIMIT_ALL)) != 0 ||
        ((resource_limits.enabled_fields &
          CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS) != 0 &&
         file_descriptor_limit < 4) ||
        ((resource_limits.enabled_fields &
          CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT) != 0 &&
         (resource_limits.cgroup_cpu_weight < 1 ||
          resource_limits.cgroup_cpu_weight > 10000)) ||
        ((resource_limits.enabled_fields &
          CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX) != 0 &&
         (resource_limits.cgroup_cpu_period_us < 1000 ||
          resource_limits.cgroup_cpu_period_us > 1000000 ||
          resource_limits.cgroup_cpu_quota_us == 0 ||
          (resource_limits.cgroup_cpu_quota_us !=
               CAPSID_RESOURCE_UNLIMITED &&
           resource_limits.cgroup_cpu_quota_us < 1000)))) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (config.sandbox_network_namespace_fd >= 0) {
#if defined(__linux__)
        if (!config.strict_sandbox ||
            ioctl(
                config.sandbox_network_namespace_fd,
                NS_GET_NSTYPE) != CLONE_NEWNET) {
            return CAPSID_INVALID_ARGUMENT;
        }
        config.sandbox_required_features |=
            CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE;
#else
        return CAPSID_INVALID_ARGUMENT;
#endif
    } else if ((config.sandbox_required_features &
                CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE) != 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (config.strict_sandbox) {
        config.sandbox_required_features |=
            CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    }
    if (cgroup_path_size != 0) {
        if (!config.strict_sandbox) {
            return CAPSID_INVALID_ARGUMENT;
        }
        config.sandbox_required_features |=
            CAPSID_SANDBOX_FEATURE_CGROUP_V2;
    }
    if ((config.sandbox_required_features &
         CAPSID_SANDBOX_FEATURE_CGROUP_V2) != 0 &&
        cgroup_path_size == 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if ((resource_limits.enabled_fields &
         kCgroupResourceLimitFields) != 0 &&
        (!config.strict_sandbox || cgroup_path_size == 0)) {
        return CAPSID_INVALID_ARGUMENT;
    }
#if defined(__APPLE__)
    if (config.process_memory_limit != 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
#endif

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return CAPSID_SYSTEM_ERROR;
    }
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    if (setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0 ||
        setsockopt(sockets[1], SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return CAPSID_SYSTEM_ERROR;
    }
#endif
    if (!set_nonblocking_cloexec(sockets[0])) {
        close(sockets[0]);
        close(sockets[1]);
        return CAPSID_SYSTEM_ERROR;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return CAPSID_SYSTEM_ERROR;
    }
    const int child_fd = 8;
    const int child_network_namespace_fd = 4;
    int network_namespace_source_fd = -1;
    if (config.sandbox_network_namespace_fd >= 0) {
        network_namespace_source_fd = fcntl(
            config.sandbox_network_namespace_fd,
            F_DUPFD_CLOEXEC,
            child_network_namespace_fd + 1);
        if (network_namespace_source_fd < 0) {
            posix_spawn_file_actions_destroy(&actions);
            close(sockets[0]);
            close(sockets[1]);
            return CAPSID_SYSTEM_ERROR;
        }
    }
    posix_spawn_file_actions_addclose(&actions, sockets[0]);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], child_fd);
    if (sockets[1] != child_fd) {
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
    }
    if (network_namespace_source_fd >= 0) {
        posix_spawn_file_actions_adddup2(
            &actions,
            network_namespace_source_fd,
            child_network_namespace_fd);
        posix_spawn_file_actions_addclose(
            &actions, network_namespace_source_fd);
    }

    const char *worker_path = config.worker_path ? config.worker_path : CAPSID_WORKER_DEFAULT_PATH;
    char fd_argument[32];
    char network_namespace_fd_argument[32];
    std::snprintf(fd_argument, sizeof(fd_argument), "%d", child_fd);
    std::snprintf(
        network_namespace_fd_argument,
        sizeof(network_namespace_fd_argument),
        "%d",
        child_network_namespace_fd);
    char *arguments[8];
    size_t argument_count = 0;
    arguments[argument_count++] = const_cast<char *>(worker_path);
    arguments[argument_count++] = const_cast<char *>("--ipc-fd");
    arguments[argument_count++] = fd_argument;
    if (network_namespace_source_fd >= 0) {
        arguments[argument_count++] =
            const_cast<char *>("--network-namespace-fd");
        arguments[argument_count++] =
            network_namespace_fd_argument;
    }
    if (config.strict_sandbox) {
        arguments[argument_count++] =
            const_cast<char *>("--close-stdio");
    }
    arguments[argument_count] = NULL;
    char *const worker_environment[] = {NULL};

    pid_t pid = -1;
    const int spawn_result = posix_spawn(
        &pid,
        worker_path,
        &actions,
        NULL,
        arguments,
        worker_environment);
    posix_spawn_file_actions_destroy(&actions);
    if (network_namespace_source_fd >= 0) {
        close(network_namespace_source_fd);
    }
    close(sockets[1]);
    if (spawn_result != 0) {
        close(sockets[0]);
        return CAPSID_CHILD_ERROR;
    }

    uint32_t preinstalled_sandbox_features = 0;
    if (cgroup_path_size != 0) {
#if defined(__linux__)
        if (!configure_and_attach_cgroup_v2(
                pid,
                config.sandbox_cgroup_path,
                resource_limits)) {
            close(sockets[0]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return CAPSID_SYSTEM_ERROR;
        }
        preinstalled_sandbox_features |=
            CAPSID_SANDBOX_FEATURE_CGROUP_V2;
#else
        close(sockets[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return CAPSID_INVALID_ARGUMENT;
#endif
    }

    capsid_worker *worker = new (std::nothrow) capsid_worker();
    if (!worker) {
        close(sockets[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return CAPSID_SYSTEM_ERROR;
    }
    worker->fd = sockets[0];
    worker->pid = pid;
    worker->closed = false;
    worker->request_timeout_ms = config.request_timeout_ms;
    worker->max_inflight_requests = config.max_inflight_requests;
    worker->max_header_bytes = config.max_header_bytes;
    worker->max_queued_bytes = config.max_queued_bytes;
    worker->write_offset = 0;
    worker->ipc_metrics_enabled = false;

    const capsid_result hello_result = send_hello(
        worker,
        config,
        validated_capability_policy,
        file_descriptor_limit,
        preinstalled_sandbox_features);
    if (hello_result != CAPSID_OK) {
        capsid_worker_destroy(worker);
        return hello_result;
    }
    *out_worker = worker;
    return CAPSID_OK;
}

void capsid_worker_destroy(capsid_worker *worker) {
    if (!worker) {
        return;
    }
    if (!worker->closed) {
        capsid_worker_shutdown(worker);
        capsid_worker_flush(worker);
    }
    if (worker->fd >= 0) {
        close(worker->fd);
        worker->fd = -1;
    }
    worker->closed = true;
    if (worker->pid > 0) {
        if (!wait_for_child(worker->pid, 100)) {
            kill(worker->pid, SIGTERM);
            if (!wait_for_child(worker->pid, 250)) {
                kill(worker->pid, SIGKILL);
                wait_for_child(worker->pid, 250);
            }
        }
    }
    delete worker;
}

int capsid_worker_fd(const capsid_worker *worker) {
    return worker && !worker->closed ? worker->fd : -1;
}

int64_t capsid_worker_pid(const capsid_worker *worker) {
    return worker ? static_cast<int64_t>(worker->pid) : -1;
}

uint32_t capsid_available_cpu_count(void) {
    const size_t count = capsid::topology::available_cpus().size();
    return count > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(count);
}

capsid_result capsid_available_cpu_at(
    uint32_t index,
    uint32_t *out_cpu) {
    if (!out_cpu) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const std::vector<uint32_t> cpus =
        capsid::topology::available_cpus();
    if (index >= cpus.size()) {
        return CAPSID_INVALID_ARGUMENT;
    }
    *out_cpu = cpus[index];
    return CAPSID_OK;
}

uint32_t capsid_recommended_worker_count(void) {
    const std::vector<uint32_t> cpus =
        capsid::topology::available_cpus();
    return capsid::topology::recommended_worker_count(
        cpus.size(),
        capsid::topology::cgroup_v2_cpu_quota());
}

capsid_result capsid_worker_set_cpu_affinity(
    capsid_worker *worker,
    uint32_t cpu) {
    if (!worker || worker->pid <= 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
#if !defined(__linux__)
    (void)cpu;
    errno = ENOTSUP;
    return CAPSID_SYSTEM_ERROR;
#else
    if (cpu >= CPU_SETSIZE) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const std::vector<uint32_t> cpus =
        capsid::topology::available_cpus();
    if (std::find(cpus.begin(), cpus.end(), cpu) == cpus.end()) {
        return CAPSID_INVALID_ARGUMENT;
    }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    return sched_setaffinity(
               worker->pid,
               sizeof(affinity),
               &affinity) == 0
        ? CAPSID_OK
        : CAPSID_SYSTEM_ERROR;
#endif
}

capsid_result capsid_worker_load_bundle(capsid_worker *worker, const uint8_t *bundle, size_t bundle_size) {
    return queue_chunked(worker, capsid::protocol::kLoadBundle, 0, bundle, bundle_size, true, true);
}

capsid_result capsid_worker_load_bundle_named(capsid_worker *worker,
                                          const uint8_t *bundle,
                                          size_t bundle_size,
                                          const char *source_name) {
    if (!source_name || !source_name[0] ||
        (bundle_size != 0 && !bundle)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const size_t name_size = std::strlen(source_name);
    if (name_size > 4096 ||
        bundle_size >
            std::numeric_limits<size_t>::max() -
                name_size - sizeof(uint16_t)) {
        return CAPSID_INVALID_ARGUMENT;
    }

    const size_t named_bundle_size =
        sizeof(uint16_t) + name_size + bundle_size;
    std::vector<uint8_t> named_bundle(named_bundle_size);
    named_bundle[0] = static_cast<uint8_t>(name_size);
    named_bundle[1] = static_cast<uint8_t>(name_size >> 8);
    std::memcpy(
        &named_bundle[sizeof(uint16_t)],
        source_name,
        name_size);
    if (bundle_size != 0) {
        std::memcpy(
            &named_bundle[sizeof(uint16_t) + name_size],
            bundle,
            bundle_size);
    }
    return queue_chunked(
        worker,
        capsid::protocol::kLoadBundle,
        0,
        &named_bundle[0],
        named_bundle.size(),
        true,
        true,
        capsid::protocol::kFlagBundleName);
}

capsid_result capsid_worker_load_trusted_bytecode_named(
    capsid_worker *worker,
    const uint8_t *bytecode,
    size_t bytecode_size,
    const char *source_name) {
    if (!source_name || !source_name[0] ||
        bytecode_size == 0 || !bytecode) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const size_t name_size = std::strlen(source_name);
    if (name_size > 4096 ||
        bytecode_size >
            std::numeric_limits<size_t>::max() -
                name_size - sizeof(uint16_t)) {
        return CAPSID_INVALID_ARGUMENT;
    }

    const size_t named_bytecode_size =
        sizeof(uint16_t) + name_size + bytecode_size;
    std::vector<uint8_t> named_bytecode(named_bytecode_size);
    named_bytecode[0] = static_cast<uint8_t>(name_size);
    named_bytecode[1] = static_cast<uint8_t>(name_size >> 8);
    std::memcpy(
        &named_bytecode[sizeof(uint16_t)],
        source_name,
        name_size);
    std::memcpy(
        &named_bytecode[sizeof(uint16_t) + name_size],
        bytecode,
        bytecode_size);
    return queue_chunked(
        worker,
        capsid::protocol::kLoadBundle,
        0,
        &named_bytecode[0],
        named_bytecode.size(),
        true,
        true,
        capsid::protocol::kFlagBundleName |
            capsid::protocol::kFlagTrustedBytecode);
}

capsid_result capsid_worker_begin_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count) {
    if (!worker || request_id == 0 || !method || !url ||
        (header_count && !headers) || header_count > std::numeric_limits<uint16_t>::max()) {
        return CAPSID_INVALID_ARGUMENT;
    }
    const size_t method_size = std::strlen(method);
    const size_t url_size = std::strlen(url);
    if (method_size == 0 || url_size == 0 ||
        method_size > std::numeric_limits<uint16_t>::max() ||
        url_size > std::numeric_limits<uint32_t>::max()) {
        return CAPSID_INVALID_ARGUMENT;
    }

    size_t payload_size =
        sizeof(uint16_t) + method_size + sizeof(uint32_t) + url_size +
        sizeof(uint16_t);
    if (payload_size > worker->max_header_bytes) {
        return CAPSID_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < header_count; ++i) {
        if (!headers[i].name.data || headers[i].name.size == 0 ||
            (!headers[i].value.data && headers[i].value.size) ||
            headers[i].name.size > std::numeric_limits<uint16_t>::max() ||
            headers[i].value.size > std::numeric_limits<uint32_t>::max()) {
            return CAPSID_INVALID_ARGUMENT;
        }
        const size_t overhead = sizeof(uint16_t) + sizeof(uint32_t);
        if (headers[i].name.size >
                worker->max_header_bytes - payload_size ||
            overhead >
                worker->max_header_bytes - payload_size -
                    headers[i].name.size ||
            headers[i].value.size >
                worker->max_header_bytes - payload_size -
                    headers[i].name.size - overhead) {
            return CAPSID_INVALID_ARGUMENT;
        }
        payload_size +=
            overhead + headers[i].name.size + headers[i].value.size;
    }

    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kRequestHead;
    frame.flags = 0;
    frame.request_id = request_id;
    append_string16(&frame.payload, reinterpret_cast<const uint8_t *>(method), method_size);
    append_string32(&frame.payload, reinterpret_cast<const uint8_t *>(url), url_size);
    capsid::protocol::append_u16(&frame.payload, static_cast<uint16_t>(header_count));
    for (size_t i = 0; i < header_count; ++i) {
        append_string16(&frame.payload, headers[i].name.data, headers[i].name.size);
        append_string32(&frame.payload, headers[i].value.data, headers[i].value.size);
    }
    if (worker->requests.find(request_id) != worker->requests.end()) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (worker->requests.size() >= worker->max_inflight_requests) {
        return CAPSID_WOULD_BLOCK;
    }
    const capsid_result result = queue_frame(worker, frame);
    if (result == CAPSID_OK) {
        forget_canceled_request(worker, request_id);
        capsid_worker::RequestState state;
        const uint64_t hard_timeout_ms =
            worker->request_timeout_ms >
                    std::numeric_limits<uint64_t>::max() -
                        kWorkerTimeoutGraceMs
                ? std::numeric_limits<uint64_t>::max()
                : worker->request_timeout_ms + kWorkerTimeoutGraceMs;
        // The worker reports a recoverable soft timeout at request_timeout_ms.
        // This later host deadline is the hard process boundary if the worker
        // cannot service its timer or interrupt handler.
        state.deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(hard_timeout_ms);
        worker->requests[request_id] = state;
    }
    return result;
}

capsid_result capsid_worker_write_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const uint8_t *data,
                                      size_t size) {
    if (!worker || request_id == 0 || (size && !data)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    std::map<uint64_t, capsid_worker::RequestState>::iterator state =
        worker->requests.find(request_id);
    if (state == worker->requests.end() || state->second.ended) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (size > state->second.credit) {
        return CAPSID_WOULD_BLOCK;
    }
    const capsid_result result =
        queue_chunked(
            worker,
            capsid::protocol::kRequestBody,
            request_id,
            data,
            size,
            false,
            false);
    if (result == CAPSID_OK) {
        state->second.credit -= size;
    }
    return result;
}

capsid_result capsid_worker_end_request(capsid_worker *worker, uint64_t request_id) {
    if (!worker || request_id == 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    std::map<uint64_t, capsid_worker::RequestState>::iterator state =
        worker->requests.find(request_id);
    if (state == worker->requests.end() || state->second.ended) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kRequestEnd;
    frame.flags = 0;
    frame.request_id = request_id;
    const capsid_result result = queue_frame(worker, frame);
    if (result == CAPSID_OK) {
        state->second.ended = true;
    }
    return result;
}

capsid_result capsid_worker_grant_response_credit(capsid_worker *worker, uint64_t request_id, uint32_t credit) {
    if (!worker || request_id == 0 || credit == 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (worker->requests.find(request_id) ==
        worker->requests.end()) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kWindowUpdate;
    frame.flags = 0;
    frame.request_id = request_id;
    capsid::protocol::append_u32(&frame.payload, credit);
    return queue_frame(worker, frame);
}

capsid_result capsid_worker_cancel(capsid_worker *worker, uint64_t request_id) {
    if (!worker || request_id == 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    std::map<uint64_t, capsid_worker::RequestState>::iterator state =
        worker->requests.find(request_id);
    if (state == worker->requests.end()) {
        return CAPSID_OK;
    }
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kCancel;
    frame.flags = 0;
    frame.request_id = request_id;
    const capsid_result result = queue_frame(worker, frame);
    if (result == CAPSID_OK) {
        remember_canceled_request(worker, request_id);
        worker->requests.erase(state);
    }
    return result;
}

capsid_result capsid_worker_request_memory_metrics(capsid_worker *worker) {
    if (!worker) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kMemoryMetricsRequest;
    frame.flags = 0;
    frame.request_id = 0;
    return queue_frame(worker, frame);
}

capsid_result capsid_worker_flush(capsid_worker *worker) {
    if (!worker || worker->closed) {
        return CAPSID_CLOSED;
    }
    if (worker->ipc_metrics_enabled) {
        ++worker->ipc_metrics.flush_calls;
    }
    while (worker->write_offset < worker->write_buffer.size()) {
        const uint8_t *data = &worker->write_buffer[worker->write_offset];
        const size_t size = worker->write_buffer.size() - worker->write_offset;
        if (worker->ipc_metrics_enabled) {
            ++worker->ipc_metrics.socket_write_calls;
        }
        const ssize_t written = write_socket(worker->fd, data, size);
        if (written > 0) {
            if (worker->ipc_metrics_enabled) {
                worker->ipc_metrics.socket_write_bytes +=
                    static_cast<uint64_t>(written);
            }
            worker->write_offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (worker->ipc_metrics_enabled) {
                ++worker->ipc_metrics.socket_write_eagain;
            }
            return CAPSID_WOULD_BLOCK;
        }
        worker->closed = true;
        close(worker->fd);
        worker->fd = -1;
        worker->requests.clear();
        worker->canceled_requests.clear();
        worker->canceled_request_order.clear();
        return CAPSID_CLOSED;
    }
    worker->write_buffer.clear();
    worker->write_offset = 0;
    return CAPSID_OK;
}

capsid_result capsid_worker_next_event(capsid_worker *worker, capsid_event *event) {
    if (!worker || !event || event->struct_size < sizeof(capsid_event)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (worker->ipc_metrics_enabled) {
        ++worker->ipc_metrics.next_event_calls;
    }
    for (;;) {
        capsid::protocol::Frame frame;
        capsid::protocol::ParseResult parse_result =
            worker->parser.next(&frame, &worker->event_payload);
        if (parse_result == capsid::protocol::kParseError) {
            return CAPSID_PROTOCOL_ERROR;
        }
        if (parse_result == capsid::protocol::kParseNeedMore) {
            uint8_t buffer[64 * 1024];
            for (;;) {
                if (worker->ipc_metrics_enabled) {
                    ++worker->ipc_metrics.socket_read_calls;
                }
                const ssize_t read_size =
                    read(worker->fd, buffer, sizeof(buffer));
                if (read_size > 0) {
                    if (worker->ipc_metrics_enabled) {
                        worker->ipc_metrics.socket_read_bytes +=
                            static_cast<uint64_t>(read_size);
                    }
                    if (!worker->parser.append(
                            buffer,
                            static_cast<size_t>(read_size))) {
                        return CAPSID_PROTOCOL_ERROR;
                    }
                    break;
                }
                if (read_size == 0) {
                    worker->closed = true;
                    worker->requests.clear();
                    worker->canceled_requests.clear();
                    worker->canceled_request_order.clear();
                    close(worker->fd);
                    worker->fd = -1;
                    event->type = CAPSID_EVENT_EXIT;
                    event->request_id = 0;
                    event->payload.data = NULL;
                    event->payload.size = 0;
                    return CAPSID_OK;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (worker->ipc_metrics_enabled) {
                        ++worker->ipc_metrics.socket_read_eagain;
                    }
                    const std::chrono::steady_clock::time_point now =
                        std::chrono::steady_clock::now();
                    for (
                        std::map<
                            uint64_t,
                            capsid_worker::RequestState>::iterator it =
                                worker->requests.begin();
                        it != worker->requests.end();
                        ++it) {
                        if (now >= it->second.deadline) {
                            const uint64_t timed_out_id = it->first;
                            kill(worker->pid, SIGKILL);
                            close(worker->fd);
                            worker->fd = -1;
                            worker->closed = true;
                            worker->requests.clear();
                            worker->canceled_requests.clear();
                            worker->canceled_request_order.clear();
                            event->type = CAPSID_EVENT_REQUEST_TIMEOUT;
                            event->request_id = timed_out_id;
                            event->flags =
                                capsid::protocol::kErrorFlagTimeout;
                            event->status = 0;
                            event->credit = 0;
                            event->payload.data = NULL;
                            event->payload.size = 0;
                            return CAPSID_OK;
                        }
                    }
                    return CAPSID_WOULD_BLOCK;
                }
                worker->closed = true;
                close(worker->fd);
                worker->fd = -1;
                return CAPSID_CLOSED;
            }
            parse_result =
                worker->parser.next(&frame, &worker->event_payload);
        }
        if (parse_result == capsid::protocol::kParseNeedMore) {
            return CAPSID_WOULD_BLOCK;
        }
        if (parse_result == capsid::protocol::kParseError) {
            return CAPSID_PROTOCOL_ERROR;
        }
        if (worker->ipc_metrics_enabled) {
            ++worker->ipc_metrics.parsed_frames;
            worker->ipc_metrics.parser_payload_copied_bytes +=
                worker->event_payload.size();
        }
        std::set<uint64_t>::iterator canceled =
            worker->canceled_requests.find(frame.request_id);
        if (canceled != worker->canceled_requests.end() &&
            is_canceled_request_frame(frame)) {
            if (frame.type == capsid::protocol::kResponseEnd ||
                frame.type == capsid::protocol::kError) {
                forget_canceled_request(worker, frame.request_id);
            }
            continue;
        }
        return map_frame_to_event(worker, frame, event);
    }
}

capsid_result capsid_worker_shutdown(capsid_worker *worker) {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kShutdown;
    frame.flags = 0;
    frame.request_id = 0;
    return queue_frame(worker, frame);
}

capsid_result capsid_worker_terminate(capsid_worker *worker) {
    if (!worker || worker->pid <= 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    return kill(worker->pid, SIGKILL) == 0 ? CAPSID_OK : CAPSID_SYSTEM_ERROR;
}

capsid_result capsid_response_header_count(const capsid_event *event,
                                       size_t *out_count) {
    if (!out_count) {
        return CAPSID_INVALID_ARGUMENT;
    }
    size_t count = 0;
    if (!decode_header_at(event, 0, &count, NULL)) {
        return CAPSID_PROTOCOL_ERROR;
    }
    *out_count = count;
    return CAPSID_OK;
}

capsid_result capsid_response_header_at(const capsid_event *event,
                                    size_t index,
                                    capsid_header *out_header) {
    if (!out_header) {
        return CAPSID_INVALID_ARGUMENT;
    }
    std::memset(out_header, 0, sizeof(*out_header));
    return decode_header_at(event, index, NULL, out_header)
               ? CAPSID_OK
               : CAPSID_PROTOCOL_ERROR;
}

capsid_result capsid_response_status_text(const capsid_event *event,
                                      capsid_bytes *out_status_text) {
    if (!out_status_text) {
        return CAPSID_INVALID_ARGUMENT;
    }
    out_status_text->data = NULL;
    out_status_text->size = 0;
    if (!event || event->type != CAPSID_EVENT_RESPONSE_HEAD ||
        !event->payload.data ||
        event->payload.size < sizeof(uint16_t) * 2) {
        return CAPSID_PROTOCOL_ERROR;
    }
    const uint8_t *cursor = event->payload.data;
    const uint8_t *end = cursor + event->payload.size;
    uint16_t size = 0;
    if (!capsid::protocol::read_u16(&cursor, end, &size) ||
        static_cast<size_t>(end - cursor) < size) {
        return CAPSID_PROTOCOL_ERROR;
    }
    out_status_text->data = size == 0 ? NULL : cursor;
    out_status_text->size = size;
    return CAPSID_OK;
}

}  // extern "C"
