#include "capsid/runtime.h"

#include "capability_policy.h"
#include "client_ipc_metrics.h"
#include "cpu_topology.h"
#include "egress_policy.h"
#include "protocol.h"
#include "response_headers.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#endif

#include <algorithm>
#include <cctype>
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
#if defined(_WIN32)
    // Worker process handle; NULL while no process is attached. The
    // public ABI reports the numeric pid via capsid_worker_pid().
    HANDLE process;
#else
    pid_t pid;
#endif
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
    // §13.2: ids of requests still inflight when the hard timeout killed
    // the worker. They drain one REQUEST_TIMEOUT event per next_event call
    // (stable map order) before the channel reports CAPSID_CLOSED.
    std::vector<uint64_t> pending_timeouts;
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
#if defined(_WIN32)
    // Windows has no FD_CLOEXEC: handles cross the process boundary only
    // through the explicit inheritable-handle list at spawn time.
    return capsid::win32::set_socket_nonblocking(fd);
#else
    const int status_flags = fcntl(fd, F_GETFL, 0);
    const int descriptor_flags = fcntl(fd, F_GETFD, 0);
    return status_flags >= 0 && descriptor_flags >= 0 &&
           fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0;
#endif
}

ssize_t write_socket(int fd, const uint8_t *data, size_t size) {
#if defined(_WIN32)
    // Winsock send() takes the raw SOCKET handle, not the CRT fd.
    return capsid::win32::send_fd(fd, data, size, 0);
#elif defined(MSG_NOSIGNAL)
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
            worker->ipc_metrics.queue_would_block.fetch_add(
                1, std::memory_order_relaxed);
        }
        return CAPSID_WOULD_BLOCK;
    }
    if (worker->write_offset && worker->write_offset == worker->write_buffer.size()) {
        worker->write_buffer.clear();
        worker->write_offset = 0;
    }
    worker->write_buffer.insert(worker->write_buffer.end(), wire.begin(), wire.end());
    if (worker->ipc_metrics_enabled) {
        worker->ipc_metrics.queued_wire_bytes.fetch_add(
            wire.size(), std::memory_order_relaxed);
        const size_t outstanding =
            worker->write_buffer.size() - worker->write_offset;
        // Atomic high-water mark (relaxed CAS update).
        size_t hw = worker->ipc_metrics.queued_bytes_high_water.load(
            std::memory_order_relaxed);
        while (hw < outstanding &&
               !worker->ipc_metrics.queued_bytes_high_water.compare_exchange_weak(
                   hw, outstanding, std::memory_order_relaxed)) {
        }
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
        worker->ipc_metrics.queued_frames.fetch_add(
            1, std::memory_order_relaxed);
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
            worker->ipc_metrics.queue_would_block.fetch_add(
                1, std::memory_order_relaxed);
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
        worker->ipc_metrics.queued_frames.fetch_add(
            frame_count, std::memory_order_relaxed);
    }
    return result;
}

// §10.3: size_t → wire integer conversions are validated here, before
// any append; on overflow nothing is written and the caller gets a
// failure instead of a truncated wire field.
bool append_string16(std::vector<uint8_t> *output,
                     const uint8_t *data,
                     size_t size) {
    if (size > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    capsid::protocol::append_u16(output, static_cast<uint16_t>(size));
    if (size != 0) {
        output->insert(output->end(), data, data + size);
    }
    return true;
}

bool append_string32(std::vector<uint8_t> *output,
                     const uint8_t *data,
                     size_t size) {
    if (size > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    capsid::protocol::append_u32(output, static_cast<uint32_t>(size));
    if (size != 0) {
        output->insert(output->end(), data, data + size);
    }
    return true;
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
    if (!append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(ca_bundle),
            std::strlen(ca_bundle))) {
        return CAPSID_INVALID_ARGUMENT;
    }
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
            if (!append_string16(
                    &frame.payload,
                    reinterpret_cast<const uint8_t *>(rule.target),
                    std::strlen(rule.target))) {
                return CAPSID_INVALID_ARGUMENT;
            }
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
    if (!append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(application_identity),
            std::strlen(application_identity))) {
        return CAPSID_INVALID_ARGUMENT;
    }
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
            if (!append_string16(
                    &frame.payload,
                    reinterpret_cast<const uint8_t *>(module),
                    std::strlen(module))) {
                return CAPSID_INVALID_ARGUMENT;
            }
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
            if (!append_string16(
                    &frame.payload,
                    reinterpret_cast<const uint8_t *>(resource),
                    std::strlen(resource))) {
                return CAPSID_INVALID_ARGUMENT;
            }
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
            if (!append_string16(
                    &frame.payload,
                    reinterpret_cast<const uint8_t *>(rule.target),
                    std::strlen(rule.target))) {
                return CAPSID_INVALID_ARGUMENT;
            }
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
        if (!append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(
                    it->first.data()),
                it->first.size())) {
            return CAPSID_INVALID_ARGUMENT;
        }
        if (!append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(
                    it->second.data()),
                it->second.size())) {
            return CAPSID_INVALID_ARGUMENT;
        }
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
            if ((frame.flags &
                 capsid::protocol::kFlagResponseFixedBody) != 0) {
                if (!capsid::protocol::read_u32(
                        &cursor, end, &event->credit) ||
                    event->credit > capsid::protocol::kMaxFixedBodySize) {
                    return CAPSID_PROTOCOL_ERROR;
                }
            }
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

#if defined(_WIN32)
bool wait_for_child(HANDLE process, uint32_t timeout_ms) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const DWORD result = WaitForSingleObject(process, 0);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result == WAIT_FAILED) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool terminate_process(HANDLE process) {
    return TerminateProcess(process, 1) != 0;
}

void kill_and_reap(HANDLE process) {
    if (!process) {
        return;
    }
    // TerminateProcess is not async-signal-like; unlike POSIX SIGKILL the
    // target cannot defer its death past the termination call itself, so
    // the bounded wait below always reaps a terminated process.
    TerminateProcess(process, 1);
    WaitForSingleObject(process, 250);
    CloseHandle(process);
}
#else
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

bool terminate_process(pid_t pid) {
    return kill(pid, SIGKILL) == 0;
}

void kill_and_reap(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
#endif

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

ClientIpcMetrics::ClientIpcMetrics() {}

void client_ipc_metrics_enable(capsid_worker *worker, bool enabled) {
    if (!worker) {
        return;
    }
    worker->ipc_metrics_enabled = enabled;
    client_ipc_metrics_reset(worker);
}

void client_ipc_metrics_reset(capsid_worker *worker) {
    if (!worker) {
        return;
    }
#define CAPSID_METRIC_STORE(field) \
    worker->ipc_metrics.field.store(0, std::memory_order_relaxed)
    CAPSID_METRIC_STORE(queued_frames);
    CAPSID_METRIC_STORE(queued_wire_bytes);
    CAPSID_METRIC_STORE(queue_would_block);
    CAPSID_METRIC_STORE(flush_calls);
    CAPSID_METRIC_STORE(socket_write_calls);
    CAPSID_METRIC_STORE(socket_write_bytes);
    CAPSID_METRIC_STORE(socket_write_eagain);
    CAPSID_METRIC_STORE(next_event_calls);
    CAPSID_METRIC_STORE(parsed_frames);
    CAPSID_METRIC_STORE(parser_payload_copied_bytes);
    CAPSID_METRIC_STORE(socket_read_calls);
    CAPSID_METRIC_STORE(socket_read_bytes);
    CAPSID_METRIC_STORE(socket_read_eagain);
    CAPSID_METRIC_STORE(queued_bytes_high_water);
#undef CAPSID_METRIC_STORE
}

bool client_ipc_metrics_snapshot(capsid_worker *worker,
                                 ClientIpcMetrics *metrics) {
    if (!worker || !metrics || !worker->ipc_metrics_enabled) {
        return false;
    }
    // Delta snapshot: exchange() reads and zeroes each counter in one
    // atomic step, so the snapshot is the delta accumulated since the
    // previous snapshot (same per-line semantics as the host metrics) and a
    // concurrent increment is never torn or lost.
#define CAPSID_METRIC_EXCHANGE(field) \
    metrics->field = \
        worker->ipc_metrics.field.exchange(0, std::memory_order_relaxed)
    CAPSID_METRIC_EXCHANGE(queued_frames);
    CAPSID_METRIC_EXCHANGE(queued_wire_bytes);
    CAPSID_METRIC_EXCHANGE(queue_would_block);
    CAPSID_METRIC_EXCHANGE(flush_calls);
    CAPSID_METRIC_EXCHANGE(socket_write_calls);
    CAPSID_METRIC_EXCHANGE(socket_write_bytes);
    CAPSID_METRIC_EXCHANGE(socket_write_eagain);
    CAPSID_METRIC_EXCHANGE(next_event_calls);
    CAPSID_METRIC_EXCHANGE(parsed_frames);
    CAPSID_METRIC_EXCHANGE(parser_payload_copied_bytes);
    CAPSID_METRIC_EXCHANGE(socket_read_calls);
    CAPSID_METRIC_EXCHANGE(socket_read_bytes);
    CAPSID_METRIC_EXCHANGE(socket_read_eagain);
    CAPSID_METRIC_EXCHANGE(queued_bytes_high_water);
#undef CAPSID_METRIC_EXCHANGE
    return true;
}

}  // namespace capsid

namespace capsid {
namespace abi {

// Thread-local, fixed-size error detail storage (WP-06, spec §10.1). The
// pointer returned by capsid_last_error() is valid until the next Capsid
// API call on the same thread; the storage never allocates and OOM paths
// store static text only.
const size_t kErrorDetailCapacity = 384;
thread_local char error_detail[kErrorDetailCapacity];
thread_local bool error_detail_set = false;

void clear_error() {
    error_detail_set = false;
}

void set_error(const char *detail) {
    if (!detail || detail[0] == '\0') {
        error_detail_set = false;
        return;
    }
    size_t i = 0;
    while (detail[i] != '\0' && i + 1 < kErrorDetailCapacity) {
        error_detail[i] = detail[i];
        ++i;
    }
    error_detail[i] = '\0';
    error_detail_set = true;
}

// §10.2: no C++ exception may cross an extern "C" boundary. std::bad_alloc
// becomes CAPSID_OUT_OF_MEMORY (only static text is stored), any other
// exception becomes CAPSID_INTERNAL_ERROR. The guard itself allocates
// nothing.
template <typename Fn>
capsid_result guard_result(Fn &&fn,
                           const char *oom_detail,
                           const char *error_detail_text) {
    clear_error();
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        set_error(oom_detail);
        return CAPSID_OUT_OF_MEMORY;
    } catch (const std::exception &exception) {
        set_error(error_detail_text ? error_detail_text
                                    : exception.what());
        return CAPSID_INTERNAL_ERROR;
    } catch (...) {
        set_error(error_detail_text);
        return CAPSID_INTERNAL_ERROR;
    }
}

// §10.2: for non-result entry points (CPU topology) an internal exception
// yields a documented conservative value plus a recorded error.
template <typename Fn, typename Value>
Value guard_value(Fn &&fn, Value conservative,
                  const char *error_detail_text) {
    clear_error();
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        set_error(error_detail_text);
        return conservative;
    } catch (const std::exception &exception) {
        set_error(error_detail_text ? error_detail_text
                                    : exception.what());
        return conservative;
    } catch (...) {
        set_error(error_detail_text);
        return conservative;
    }
}

// §10.3: checked accumulation helpers. Every reserve/insert/strlen
// accumulation before a payload write goes through these; overflow is an
// INVALID_ARGUMENT-class failure for the caller.
bool checked_add(size_t a, size_t b, size_t *out) {
    if (a > std::numeric_limits<size_t>::max() - b) {
        return false;
    }
    *out = a + b;
    return true;
}

bool checked_mul(size_t a, size_t b, size_t *out) {
    if (b != 0 && a > std::numeric_limits<size_t>::max() / b) {
        return false;
    }
    *out = a * b;
    return true;
}

}  // namespace abi
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
#if defined(__APPLE__) || defined(CAPSID_ASAN_BUILD) || \
    defined(CAPSID_TSAN_BUILD)
    // macOS does not support the Linux address-space gate. ASan/TSan also
    // reserve large shadow ranges that are incompatible with the production
    // default; their instrumented builds still enforce js_heap_limit inside
    // QuickJS.
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
        case CAPSID_OUT_OF_MEMORY:
            return "out of memory";
        case CAPSID_INTERNAL_ERROR:
            return "internal error";
    }
    return "unknown result";
}

const char *capsid_last_error(void) {
    return capsid::abi::error_detail_set
               ? capsid::abi::error_detail
               : NULL;
}

capsid_result capsid_worker_spawn(const capsid_worker_config *input, capsid_worker **out_worker) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
            // §10.3: checked accumulation before any payload sizing.
            size_t rule_size = 0;
            if (!capsid::abi::checked_add(
                    static_cast<size_t>(14),
                    std::strlen(
                        config.egress_policy->rules[index].target),
                    &rule_size) ||
                !capsid::abi::checked_add(
                    egress_rule_payload_size, rule_size,
                    &egress_rule_payload_size)) {
                return CAPSID_INVALID_ARGUMENT;
            }
        }
    }
    size_t capability_payload_size = 0;
    if (config.capability_policy) {
        const capsid_capability_policy &capability =
            *config.capability_policy;
        if (!capsid::abi::checked_add(
                capability_payload_size,
                std::strlen(
                    capability.application_identity
                        ? capability.application_identity
                        : ""),
                &capability_payload_size)) {
            return CAPSID_INVALID_ARGUMENT;
        }
        for (uint32_t index = 0;
             index < capability.allowed_module_count;
             ++index) {
            size_t module_size = 0;
            if (!capsid::abi::checked_add(
                    static_cast<size_t>(2),
                    std::strlen(capability.allowed_modules[index]),
                    &module_size) ||
                !capsid::abi::checked_add(
                    capability_payload_size, module_size,
                    &capability_payload_size)) {
                return CAPSID_INVALID_ARGUMENT;
            }
        }
        for (uint32_t index = 0;
             index < capability.rule_count;
             ++index) {
            size_t rule_size = 0;
            if (!capsid::abi::checked_add(
                    static_cast<size_t>(14),
                    std::strlen(
                        capability.rules[index].resource
                            ? capability.rules[index].resource
                            : ""),
                    &rule_size) ||
                !capsid::abi::checked_add(
                    capability_payload_size, rule_size,
                    &capability_payload_size)) {
                return CAPSID_INVALID_ARGUMENT;
            }
        }
        if (capability.net_policy) {
            for (uint32_t index = 0;
                 index < capability.net_policy->rule_count;
                 ++index) {
                size_t rule_size = 0;
                if (!capsid::abi::checked_add(
                        static_cast<size_t>(14),
                        std::strlen(
                            capability.net_policy
                                ->rules[index]
                                .target),
                        &rule_size) ||
                    !capsid::abi::checked_add(
                        capability_payload_size, rule_size,
                        &capability_payload_size)) {
                    return CAPSID_INVALID_ARGUMENT;
                }
            }
        }
    }
    const std::vector<
        std::pair<std::string, std::string> >
        &validated_environment =
            validated_capability_policy.env_entries();
    if (!capsid::abi::checked_add(
            capability_payload_size, static_cast<size_t>(2),
            &capability_payload_size)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    for (std::vector<
             std::pair<std::string, std::string> >::const_iterator
             it = validated_environment.begin();
         it != validated_environment.end();
         ++it) {
        size_t entry_size = 0;
        if (!capsid::abi::checked_add(
                static_cast<size_t>(4),
                it->first.size(), &entry_size) ||
            !capsid::abi::checked_add(
                entry_size, it->second.size(), &entry_size) ||
            !capsid::abi::checked_add(
                capability_payload_size, entry_size,
                &capability_payload_size)) {
            return CAPSID_INVALID_ARGUMENT;
        }
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
#if defined(_WIN32)
    // Strict sandbox has no Windows equivalent (no seccomp/Landlock;
    // see docs/windows.md). Reject at spawn so the worker never starts
    // under a configuration it cannot enforce.
    if (config.strict_sandbox) {
        return CAPSID_INVALID_ARGUMENT;
    }
#endif
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
#if defined(_WIN32)
    // Windows worker IPC: a loopback TCP socket pair replaces
    // socketpair(AF_UNIX, SOCK_STREAM). Both ends become CRT fds so the
    // shared read()/send() paths below stay identical to POSIX. Only the
    // child end is marked inheritable, and it is the only handle passed
    // through CreateProcess's explicit handle list — no other parent
    // descriptor crosses the process boundary (stdio is added to the list
    // only when --close-stdio is absent).
    if (!capsid::win32::ensure_winsock()) {
        return CAPSID_SYSTEM_ERROR;
    }
    {
        const SOCKET listener =
            socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            return CAPSID_SYSTEM_ERROR;
        }
        SOCKET child = INVALID_SOCKET;
        SOCKET parent = INVALID_SOCKET;
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t address_size = sizeof(address);
        if (bind(listener,
                 reinterpret_cast<const struct sockaddr *>(&address),
                 sizeof(address)) != 0 ||
            listen(listener, 1) != 0 ||
            getsockname(listener,
                        reinterpret_cast<struct sockaddr *>(&address),
                        &address_size) != 0 ||
            (child = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) ==
                INVALID_SOCKET ||
            connect(child,
                    reinterpret_cast<const struct sockaddr *>(&address),
                    sizeof(address)) != 0 ||
            (parent = accept(listener, NULL, NULL)) ==
                INVALID_SOCKET) {
            if (child != INVALID_SOCKET) {
                closesocket(child);
            }
            if (parent != INVALID_SOCKET) {
                closesocket(parent);
            }
            closesocket(listener);
            return CAPSID_SYSTEM_ERROR;
        }
        closesocket(listener);
        if (!SetHandleInformation(reinterpret_cast<HANDLE>(child),
                                  HANDLE_FLAG_INHERIT,
                                  HANDLE_FLAG_INHERIT)) {
            closesocket(child);
            closesocket(parent);
            return CAPSID_SYSTEM_ERROR;
        }
        sockets[0] = _open_osfhandle(
            static_cast<intptr_t>(parent), _O_RDWR | _O_BINARY);
        sockets[1] = _open_osfhandle(
            static_cast<intptr_t>(child), _O_RDWR | _O_BINARY);
        if (sockets[0] < 0 || sockets[1] < 0) {
            if (sockets[0] >= 0) {
                close(sockets[0]);
            }
            if (sockets[1] >= 0) {
                close(sockets[1]);
            }
            return CAPSID_SYSTEM_ERROR;
        }
    }
    if (!set_nonblocking_cloexec(sockets[0])) {
        close(sockets[0]);
        close(sockets[1]);
        return CAPSID_SYSTEM_ERROR;
    }
#else
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
        // Reserve the namespace-fd copy strictly above every descriptor the
        // child will receive (8 = IPC, 4 = namespace): the parent's fd table
        // is not under our control, and a runner like ctest leaves extra
        // descriptors open, which can push the F_DUPFD result onto the IPC
        // dup2 target and clobber the IPC socket during the pre-exec file
        // actions (observed as an immediate worker exit on read(8)).
        const int fd_floor =
            std::max(child_fd, child_network_namespace_fd) + 1;
        network_namespace_source_fd = fcntl(
            config.sandbox_network_namespace_fd,
            F_DUPFD_CLOEXEC,
            fd_floor);
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
#endif

    const char *worker_path = config.worker_path ? config.worker_path : CAPSID_WORKER_DEFAULT_PATH;
    char fd_argument[32];
    std::snprintf(
        fd_argument,
        sizeof(fd_argument),
#if defined(_WIN32)
        // The child inherits the same OS handle value; pass it through the
        // command line the way POSIX passes the dup2 target fd number.
        "%llu",
        static_cast<unsigned long long>(
            static_cast<uintptr_t>(_get_osfhandle(sockets[1]))));
#else
        "%d",
        child_fd);
#endif
#if defined(_WIN32)
    // CreateProcessA takes the command line as one string. Every argument
    // here is a bareword (--ipc-fd, the numeric fd, --close-stdio), and the
    // worker path is quoted with embedded quotes doubled.
    std::string command_line = "\"";
    for (const char *cursor = worker_path; *cursor != '\0'; ++cursor) {
        command_line.push_back(*cursor);
        if (*cursor == '"') {
            command_line.push_back('"');
        }
    }
    command_line += "\" --ipc-fd ";
    command_line += fd_argument;
    if (config.strict_sandbox) {
        command_line += " --close-stdio";
    }
#else
    char network_namespace_fd_argument[32];
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
#endif

#if defined(_WIN32)
    // Forward CAPSID_* environment to the worker (diagnostic controls
    // like CAPSID_PERF_DIAG); nothing else leaks across the boundary.
    // CreateProcess requires the block to be sorted case-insensitively.
    std::vector<std::string> capsid_environment;
    // "environ" is a macro on MSVC (__p__environ()); use a clean local
    // name for the block source.
    char **environment_source = _environ;
    for (char **env = environment_source;
         env != NULL && *env != NULL;
         ++env) {
        if (std::strncmp(*env, "CAPSID_", 7) == 0) {
            capsid_environment.push_back(*env);
        }
    }
    std::sort(
        capsid_environment.begin(),
        capsid_environment.end(),
        [](const std::string &left, const std::string &right) {
            const size_t limit = std::min(left.size(), right.size());
            for (size_t i = 0; i < limit; ++i) {
                const char left_character =
                    std::tolower(static_cast<unsigned char>(left[i]));
                const char right_character =
                    std::tolower(static_cast<unsigned char>(right[i]));
                if (left_character != right_character) {
                    return left_character < right_character;
                }
            }
            return left.size() < right.size();
        });
    std::string environment_block;
    for (size_t index = 0; index < capsid_environment.size(); ++index) {
        environment_block += capsid_environment[index];
        environment_block.push_back('\0');
    }

    // EXTENDED_STARTUPINFO_PRESENT + PROC_THREAD_ATTRIBUTE_HANDLE_LIST:
    // only the listed inheritable handles cross the process boundary.
    STARTUPINFOEXA startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    HANDLE inherited_handles[4];
    SIZE_T inherited_count = 0;
    inherited_handles[inherited_count++] =
        reinterpret_cast<HANDLE>(_get_osfhandle(sockets[1]));
    if (!config.strict_sandbox) {
        // Preserve stdio inheritance parity with POSIX spawn (no
        // --close-stdio): the worker's stderr diagnostics stay visible.
        const HANDLE standard_handles[3] = {
            GetStdHandle(STD_INPUT_HANDLE),
            GetStdHandle(STD_OUTPUT_HANDLE),
            GetStdHandle(STD_ERROR_HANDLE)};
        for (size_t index = 0; index < 3; ++index) {
            if (standard_handles[index] != NULL &&
                standard_handles[index] != INVALID_HANDLE_VALUE) {
                inherited_handles[inherited_count++] =
                    standard_handles[index];
            }
        }
    }
    SIZE_T attribute_size = 0;
    (void)InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attribute_size));
    if (startup.lpAttributeList == NULL ||
        !InitializeProcThreadAttributeList(
            startup.lpAttributeList, 1, 0, &attribute_size) ||
        !UpdateProcThreadAttribute(
            startup.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles,
            inherited_count * sizeof(HANDLE),
            NULL,
            NULL)) {
        if (startup.lpAttributeList != NULL) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        }
        close(sockets[0]);
        close(sockets[1]);
        return CAPSID_SYSTEM_ERROR;
    }
    PROCESS_INFORMATION process_info;
    std::memset(&process_info, 0, sizeof(process_info));
    const BOOL created = CreateProcessA(
        worker_path,
        &command_line[0],
        NULL,
        NULL,
        TRUE,
        EXTENDED_STARTUPINFO_PRESENT,
        environment_block.empty()
            ? NULL
            : reinterpret_cast<LPVOID>(&environment_block[0]),
        NULL,
        &startup.StartupInfo,
        &process_info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    close(sockets[1]);
    if (!created) {
        close(sockets[0]);
        return CAPSID_CHILD_ERROR;
    }
    CloseHandle(process_info.hThread);
    // Named "pid" like the POSIX branch so the shared post-spawn paths
    // (cgroup rejection, worker construction, reap-on-failure) compile
    // against one identifier: a HANDLE here, pid_t elsewhere.
    HANDLE pid = process_info.hProcess;
#else
    extern char **environ;
    // Forward CAPSID_* environment to the worker (diagnostic controls
    // like CAPSID_PERF_DIAG); nothing else leaks across the boundary.
    char **worker_environment = NULL;
    size_t capsid_env_count = 0;
    for (char **env = environ; env != NULL && *env != NULL; ++env) {
        if (std::strncmp(*env, "CAPSID_", 7) == 0) {
            capsid_env_count += 1;
        }
    }
    if (capsid_env_count != 0) {
        worker_environment = static_cast<char **>(
            std::calloc(capsid_env_count + 1, sizeof(char *)));
        size_t index = 0;
        for (char **env = environ; env != NULL && *env != NULL; ++env) {
            if (std::strncmp(*env, "CAPSID_", 7) == 0) {
                worker_environment[index++] = *env;
            }
        }
        worker_environment[index] = NULL;
    }

    pid_t pid = -1;
    const int spawn_result = posix_spawn(
        &pid,
        worker_path,
        &actions,
        NULL,
        arguments,
        worker_environment);
    if (worker_environment != NULL) {
        std::free(worker_environment);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (network_namespace_source_fd >= 0) {
        close(network_namespace_source_fd);
    }
    close(sockets[1]);
    if (spawn_result != 0) {
        close(sockets[0]);
        return CAPSID_CHILD_ERROR;
    }
#endif

    uint32_t preinstalled_sandbox_features = 0;
    if (cgroup_path_size != 0) {
#if defined(__linux__)
        bool cgroup_attached = false;
        try {
            cgroup_attached = configure_and_attach_cgroup_v2(
                pid,
                config.sandbox_cgroup_path,
                resource_limits);
        } catch (...) {
            // WP-06: an allocating failure inside cgroup attach must not
            // leak the freshly forked child.
            close(sockets[0]);
            kill_and_reap(pid);
            throw;
        }
        if (!cgroup_attached) {
            close(sockets[0]);
            kill_and_reap(pid);
            return CAPSID_SYSTEM_ERROR;
        }
        preinstalled_sandbox_features |=
            CAPSID_SANDBOX_FEATURE_CGROUP_V2;
#else
        close(sockets[0]);
        kill_and_reap(pid);
        return CAPSID_INVALID_ARGUMENT;
#endif
    }

    // WP-06, spec §10.4: a failing spawn must never leave a half-built
    // worker behind. The output is cleared before the body runs so every
    // failure path — including one that throws into the ABI guard — has a
    // well-defined out_worker value.
    *out_worker = NULL;

    // WP-06, spec §10.4: std::nothrow only protects the operator new
    // call itself. The capsid_worker members (vector/map/set/deque/Parser)
    // allocate in the constructor, and a bad_alloc from those propagates
    // past the nothrow guard — the child would leak alive if this were not
    // caught and reaped. The new expression itself runs the matching
    // operator delete when the constructor throws, so only the child and
    // the IPC descriptor need cleanup here.
    capsid_worker *worker = NULL;
    try {
        worker = new (std::nothrow) capsid_worker();
    } catch (const std::bad_alloc &) {
        close(sockets[0]);
        kill_and_reap(pid);
        // Uniform OOM contract: same code and detail the ABI guard would
        // have produced.
        capsid::abi::set_error("capsid_worker_spawn: out of memory");
        return CAPSID_OUT_OF_MEMORY;
    }
    if (!worker) {
        close(sockets[0]);
        kill_and_reap(pid);
        // WP-06: a nothrow allocation failure is an OOM; report it with
        // the same code and detail the ABI guard would have produced, so
        // callers see a uniform CAPSID_OUT_OF_MEMORY contract.
        capsid::abi::set_error("capsid_worker_spawn: out of memory");
        return CAPSID_OUT_OF_MEMORY;
    }
    worker->fd = sockets[0];
#if defined(_WIN32)
    worker->process = pid;
#else
    worker->pid = pid;
#endif
    worker->closed = false;
    worker->request_timeout_ms = config.request_timeout_ms;
    worker->max_inflight_requests = config.max_inflight_requests;
    worker->max_header_bytes = config.max_header_bytes;
    worker->max_queued_bytes = config.max_queued_bytes;
    worker->write_offset = 0;
    worker->ipc_metrics_enabled = false;

    // WP-06, spec §10.2/§10.4: allocations between fork and the first
    // wire frame (cgroup attach, hello encode/queue) may throw; the child
    // must be reaped before the exception reaches the ABI guard, or it
    // leaks as a zombie.
    capsid_result hello_result = CAPSID_INTERNAL_ERROR;
    try {
        hello_result = send_hello(
            worker,
            config,
            validated_capability_policy,
            file_descriptor_limit,
            preinstalled_sandbox_features);
    } catch (...) {
        close(sockets[0]);
        kill_and_reap(pid);
        delete worker;
        throw;
    }
    if (hello_result != CAPSID_OK) {
        capsid_worker_destroy(worker);
        return hello_result;
    }
    *out_worker = worker;
    return CAPSID_OK;
        },
        "capsid_worker_spawn: out of memory",
        "capsid_worker_spawn: internal error");
}

void capsid_worker_destroy(capsid_worker *worker) {
    if (!worker) {
        return;
    }
    // §13.3: abortive cleanup. destroy never attempts graceful shutdown —
    // a caller who wants a graceful stop must run the documented sequence
    // shutdown → flush → drain EXIT → destroy explicitly (the Host's
    // normal-stop path does). In-flight requests are terminated without
    // warning; the worker is SIGKILLed (cooperative SIGTERM is not part of
    // the abortive contract). Teardown allocates nothing and never throws
    // (spec §10.2): close, signal and reap below are unconditional, and
    // the child is always reaped before destroy returns.
    if (worker->fd >= 0) {
        close(worker->fd);
        worker->fd = -1;
    }
    worker->closed = true;
#if defined(_WIN32)
    if (worker->process != NULL) {
        // A short natural-exit window keeps the reap cheap for workers that
        // are already gone; anything still alive is terminated and reaped.
        if (!wait_for_child(worker->process, 50)) {
            terminate_process(worker->process);
            wait_for_child(worker->process, 250);
        }
        CloseHandle(worker->process);
        worker->process = NULL;
    }
#else
    if (worker->pid > 0) {
        // A short natural-exit window keeps the reap cheap for workers that
        // are already gone; anything still alive is SIGKILLed and reaped.
        if (!wait_for_child(worker->pid, 50)) {
            kill(worker->pid, SIGKILL);
            wait_for_child(worker->pid, 250);
        }
    }
#endif
    delete worker;
}

int capsid_worker_fd(const capsid_worker *worker) {
    return worker && !worker->closed ? worker->fd : -1;
}

int64_t capsid_worker_pid(const capsid_worker *worker) {
#if defined(_WIN32)
    return worker && worker->process != NULL
        ? static_cast<int64_t>(GetProcessId(worker->process))
        : -1;
#else
    return worker ? static_cast<int64_t>(worker->pid) : -1;
#endif
}

static uint32_t capsid_available_cpu_count_impl(void);
static uint32_t capsid_recommended_worker_count_impl(void);

uint32_t capsid_available_cpu_count(void) {
    // Frozen conservative fallback (spec §10.2): 1 CPU on internal error,
    // with capsid_last_error() set. Documented in runtime.h.
    return capsid::abi::guard_value(
        &capsid_available_cpu_count_impl,
        /*conservative=*/1,
        "capsid_available_cpu_count: topology query failed");
}

static uint32_t capsid_available_cpu_count_impl(void) {
    const size_t count = capsid::topology::available_cpus().size();
    return count > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(count);
}

capsid_result capsid_available_cpu_at(
    uint32_t index,
    uint32_t *out_cpu) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_available_cpu_at: out of memory",
        "capsid_available_cpu_at: topology query failed");
}

uint32_t capsid_recommended_worker_count(void) {
    // Frozen conservative fallback (spec §10.2): 1 worker on internal
    // error, with capsid_last_error() set. Documented in runtime.h.
    return capsid::abi::guard_value(
        &capsid_recommended_worker_count_impl,
        /*conservative=*/1,
        "capsid_recommended_worker_count: topology query failed");
}

static uint32_t capsid_recommended_worker_count_impl(void) {
    const std::vector<uint32_t> cpus =
        capsid::topology::available_cpus();
    return capsid::topology::recommended_worker_count(
        cpus.size(),
        capsid::topology::cgroup_v2_cpu_quota());
}

capsid_result capsid_worker_set_cpu_affinity(
    capsid_worker *worker,
    uint32_t cpu) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
#if defined(_WIN32)
    if (!worker || worker->process == NULL) {
        return CAPSID_INVALID_ARGUMENT;
    }
#else
    if (!worker || worker->pid <= 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
#endif
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
        },
        "capsid_worker_set_cpu_affinity: out of memory",
        "capsid_worker_set_cpu_affinity: internal error");
}

capsid_result capsid_worker_load_bundle(capsid_worker *worker, const uint8_t *bundle, size_t bundle_size) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    return queue_chunked(worker, capsid::protocol::kLoadBundle, 0, bundle, bundle_size, true, true);
        },
        "capsid_worker_load_bundle: out of memory",
        "capsid_worker_load_bundle: internal error");
}

capsid_result capsid_worker_load_bundle_named(capsid_worker *worker,
                                          const uint8_t *bundle,
                                          size_t bundle_size,
                                          const char *source_name) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_load_bundle_named: out of memory",
        "capsid_worker_load_bundle_named: internal error");
}

capsid_result capsid_worker_load_trusted_bytecode_named(
    capsid_worker *worker,
    const uint8_t *bytecode,
    size_t bytecode_size,
    const char *source_name) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_load_trusted_bytecode_named: out of memory",
        "capsid_worker_load_trusted_bytecode_named: internal error");
}

static capsid_result begin_request_impl(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count,
                                      uint32_t flags) {
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
    frame.flags = flags;
    frame.request_id = request_id;
    if (!append_string16(&frame.payload, reinterpret_cast<const uint8_t *>(method), method_size)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (!append_string32(&frame.payload, reinterpret_cast<const uint8_t *>(url), url_size)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::protocol::append_u16(&frame.payload, static_cast<uint16_t>(header_count));
    for (size_t i = 0; i < header_count; ++i) {
        if (!append_string16(&frame.payload, headers[i].name.data, headers[i].name.size)) {
            return CAPSID_INVALID_ARGUMENT;
        }
        if (!append_string32(&frame.payload, headers[i].value.data, headers[i].value.size)) {
            return CAPSID_INVALID_ARGUMENT;
        }
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
        state.ended = (flags & capsid::protocol::kFlagRequestEnd) != 0;
        const uint64_t hard_timeout_ms =
            worker->request_timeout_ms >
                    std::numeric_limits<uint64_t>::max() -
                        kWorkerTimeoutGraceMs
                ? std::numeric_limits<uint64_t>::max()
                : worker->request_timeout_ms + kWorkerTimeoutGraceMs;
        state.deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(hard_timeout_ms);
        worker->requests[request_id] = state;
    }
    return result;
}

// begin_request with kFlagRequestEnd: worker marks request_ended
// immediately and skips initial request-direction credit.
capsid_result capsid_worker_begin_bodyless_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    // Reuse the validation and frame assembly of begin_request by setting
    // the flag; the shared impl below handles it.
    return begin_request_impl(worker, request_id, method, url,
                              headers, header_count,
                              capsid::protocol::kFlagRequestEnd);
        },
        "capsid_worker_begin_bodyless_request: out of memory",
        "capsid_worker_begin_bodyless_request: internal error");
}

capsid_result capsid_worker_begin_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    return begin_request_impl(worker, request_id, method, url,
                              headers, header_count, 0);
        },
        "capsid_worker_begin_request: out of memory",
        "capsid_worker_begin_request: internal error");
}

capsid_result capsid_worker_write_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const uint8_t *data,
                                      size_t size) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_write_request: out of memory",
        "capsid_worker_write_request: internal error");
}

capsid_result capsid_worker_end_request(capsid_worker *worker, uint64_t request_id) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_end_request: out of memory",
        "capsid_worker_end_request: internal error");
}

capsid_result capsid_worker_grant_response_credit(capsid_worker *worker, uint64_t request_id, uint32_t credit) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_grant_response_credit: out of memory",
        "capsid_worker_grant_response_credit: internal error");
}

capsid_result capsid_worker_cancel(capsid_worker *worker, uint64_t request_id) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
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
        },
        "capsid_worker_cancel: out of memory",
        "capsid_worker_cancel: internal error");
}

capsid_result capsid_worker_request_memory_metrics(capsid_worker *worker) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    if (!worker) {
        return CAPSID_INVALID_ARGUMENT;
    }
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kMemoryMetricsRequest;
    frame.flags = 0;
    frame.request_id = 0;
    return queue_frame(worker, frame);
        },
        "capsid_worker_request_memory_metrics: out of memory",
        "capsid_worker_request_memory_metrics: internal error");
}

capsid_result capsid_worker_flush(capsid_worker *worker) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    if (!worker || worker->closed) {
        return CAPSID_CLOSED;
    }
    if (worker->ipc_metrics_enabled) {
        worker->ipc_metrics.flush_calls.fetch_add(
                    1, std::memory_order_relaxed);
    }
    while (worker->write_offset < worker->write_buffer.size()) {
        const uint8_t *data = &worker->write_buffer[worker->write_offset];
        const size_t size = worker->write_buffer.size() - worker->write_offset;
        if (worker->ipc_metrics_enabled) {
            worker->ipc_metrics.socket_write_calls.fetch_add(
                    1, std::memory_order_relaxed);
        }
        const ssize_t written = write_socket(worker->fd, data, size);
        if (written > 0) {
            if (worker->ipc_metrics_enabled) {
                worker->ipc_metrics.socket_write_bytes.fetch_add(
                    static_cast<uint64_t>(written), std::memory_order_relaxed);
            }
            worker->write_offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (worker->ipc_metrics_enabled) {
                worker->ipc_metrics.socket_write_eagain.fetch_add(
                    1, std::memory_order_relaxed);
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
        },
        "capsid_worker_flush: out of memory",
        "capsid_worker_flush: internal error");
}

capsid_result capsid_worker_next_event(capsid_worker *worker, capsid_event *event) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    if (!worker || !event || event->struct_size < sizeof(capsid_event)) {
        return CAPSID_INVALID_ARGUMENT;
    }
    if (worker->ipc_metrics_enabled) {
        worker->ipc_metrics.next_event_calls.fetch_add(
                    1, std::memory_order_relaxed);
    }
    // §13.2: the hard timeout killed the worker with requests inflight;
    // deliver the remaining terminal reasons before reporting the closed
    // channel. The construction mirrors the timeout branch (all fields
    // deterministic).
    if (worker->closed && !worker->pending_timeouts.empty()) {
        event->type = CAPSID_EVENT_REQUEST_TIMEOUT;
        event->request_id = worker->pending_timeouts.front();
        worker->pending_timeouts.erase(worker->pending_timeouts.begin());
        event->flags = capsid::protocol::kErrorFlagTimeout;
        event->status = 0;
        event->credit = 0;
        event->payload.data = NULL;
        event->payload.size = 0;
        return CAPSID_OK;
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
                    worker->ipc_metrics.socket_read_calls.fetch_add(
                    1, std::memory_order_relaxed);
                }
                const ssize_t read_size =
                    read(worker->fd, buffer, sizeof(buffer));
                if (read_size > 0) {
                    if (worker->ipc_metrics_enabled) {
                        worker->ipc_metrics.socket_read_bytes.fetch_add(
                            static_cast<uint64_t>(read_size), std::memory_order_relaxed);
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
                    // §13.1: every event construction must leave the unused
                    // fields at a deterministic value. next_event writes
                    // only what an event needs, so a reuser of one
                    // capsid_event would otherwise see a previous event's
                    // flags/status/credit on EXIT (REQUEST_TIMEOUT zeroes
                    // all three; EXIT must match).
                    event->type = CAPSID_EVENT_EXIT;
                    event->request_id = 0;
                    event->flags = 0;
                    event->status = 0;
                    event->credit = 0;
                    event->payload.data = NULL;
                    event->payload.size = 0;
                    return CAPSID_OK;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (worker->ipc_metrics_enabled) {
                        worker->ipc_metrics.socket_read_eagain.fetch_add(
                    1, std::memory_order_relaxed);
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
                            terminate_process(
#if defined(_WIN32)
                                worker->process);
#else
                                worker->pid);
#endif
                            close(worker->fd);
                            worker->fd = -1;
                            worker->closed = true;
                            // §13.2: every request still inflight is gone
                            // with the killed worker and must receive a
                            // terminal reason. The timed-out id is reported
                            // first; the rest drain as successive
                            // REQUEST_TIMEOUT events in stable map order.
                            worker->pending_timeouts.clear();
                            for (const auto &entry : worker->requests) {
                                if (entry.first != timed_out_id) {
                                    worker->pending_timeouts.push_back(
                                        entry.first);
                                }
                            }
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
            worker->ipc_metrics.parsed_frames.fetch_add(
                    1, std::memory_order_relaxed);
            worker->ipc_metrics.parser_payload_copied_bytes.fetch_add(
                worker->event_payload.size(), std::memory_order_relaxed);
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
        },
        "capsid_worker_next_event: out of memory",
        "capsid_worker_next_event: internal error");
}

capsid_result capsid_worker_shutdown(capsid_worker *worker) {
    return capsid::abi::guard_result(
        [&]() -> capsid_result {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kShutdown;
    frame.flags = 0;
    frame.request_id = 0;
    return queue_frame(worker, frame);
        },
        "capsid_worker_shutdown: out of memory",
        "capsid_worker_shutdown: internal error");
}

capsid_result capsid_worker_terminate(capsid_worker *worker) {
#if defined(_WIN32)
    if (!worker || worker->process == NULL) {
        return CAPSID_INVALID_ARGUMENT;
    }
    return terminate_process(worker->process)
        ? CAPSID_OK
        : CAPSID_SYSTEM_ERROR;
#else
    if (!worker || worker->pid <= 0) {
        return CAPSID_INVALID_ARGUMENT;
    }
    return kill(worker->pid, SIGKILL) == 0 ? CAPSID_OK : CAPSID_SYSTEM_ERROR;
#endif
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
