#include "capsid/runtime.h"

#include "win32_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

typedef std::pair<std::string, std::string> Header;

struct Response {
    uint32_t status;
    std::string status_text;
    std::vector<Header> headers;
    std::vector<uint8_t> body;
    std::string error;

    Response() : status(0) {}
};

// Set from the --collect-events driver flag; gates EVENTS emission after
// each RESULT/CANCELED/CANCELED_UPLOAD line. Collection always happens.
bool collect_events = false;

std::string hex(const uint8_t *data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    if (size == 0) {
        return "-";
    }
    std::string output;
    output.reserve(size * 2);
    for (size_t index = 0; index < size; ++index) {
        output.push_back(digits[data[index] >> 4]);
        output.push_back(digits[data[index] & 0x0f]);
    }
    return output;
}

std::string hex(const std::string &value) {
    return hex(
        reinterpret_cast<const uint8_t *>(value.data()),
        value.size());
}

std::string bytes(const capsid_bytes &value) {
    return value.size == 0
        ? std::string()
        : std::string(reinterpret_cast<const char *>(value.data), value.size);
}

std::string log_message(const capsid_event &event) {
    if (event.payload.size < 2) {
        return std::string();
    }
    const size_t stream_size = event.payload.data[0] |
                               (static_cast<size_t>(event.payload.data[1]) << 8);
    if (stream_size > event.payload.size - 2) {
        return std::string();
    }
    const char *data = reinterpret_cast<const char *>(event.payload.data);
    return std::string(data + 2 + stream_size,
                       event.payload.size - 2 - stream_size);
}

// One native event observed on the wire while a request was in flight.
// For LOG events `text` is the message; for AUDIT events `text` is the
// capability, `resource` the resource, and `record_id` the request id
// embedded in the decoded audit record.
struct NativeEvent {
    bool audit;
    uint64_t request_id;
    uint64_t record_id;
    std::string text;
    std::string resource;

    NativeEvent()
        : audit(false), request_id(0), record_id(0) {}
};

typedef std::vector<NativeEvent> NativeEvents;

void collect_native_event(const capsid_event &event, NativeEvents *events) {
    if (event.type == CAPSID_EVENT_LOG) {
        NativeEvent item;
        item.audit = false;
        item.request_id = event.request_id;
        item.record_id = 0;
        item.text = log_message(event);
        item.resource.clear();
        events->push_back(item);
        return;
    }
    if (event.type == CAPSID_EVENT_AUDIT) {
        capsid_audit_record audit;
        capsid_audit_record_init(&audit);
        if (capsid_audit_record_decode(&event, &audit) != CAPSID_OK) {
            return;
        }
        NativeEvent item;
        item.audit = true;
        item.request_id = event.request_id;
        item.record_id = audit.request_id;
        item.text = bytes(audit.capability);
        item.resource = bytes(audit.resource);
        events->push_back(item);
        return;
    }
}

// Emitted only when --collect-events is on. Foreign events (request_id 0 or
// any id that is not the owning request's) are attached to the first block
// that asks for them so they always surface and never get dropped.
void emit_events_block(uint64_t request_id, const NativeEvents &events) {
    if (!collect_events) {
        return;
    }
    std::vector<const NativeEvent *> block;
    for (size_t index = 0; index < events.size(); ++index) {
        if (events[index].request_id == request_id ||
            events[index].request_id == 0) {
            block.push_back(&events[index]);
        }
    }
    std::cout << "EVENTS " << request_id << " " << block.size() << std::endl;
    for (size_t index = 0; index < block.size(); ++index) {
        const NativeEvent &item = *block[index];
        if (!item.audit) {
            std::cout << "LOG " << item.request_id << " "
                      << hex(item.text) << std::endl;
        } else {
            std::cout << "AUDIT " << item.request_id << " "
                      << item.record_id << " "
                      << hex(item.text) << " "
                      << hex(item.resource) << std::endl;
        }
    }
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool unhex(const std::string &text, std::vector<uint8_t> *output) {
    output->clear();
    if (text == "-") {
        return true;
    }
    if ((text.size() % 2) != 0) {
        return false;
    }
    output->reserve(text.size() / 2);
    for (size_t index = 0; index < text.size(); index += 2) {
        const int high = hex_digit(text[index]);
        const int low = hex_digit(text[index + 1]);
        if (high < 0 || low < 0) {
            output->clear();
            return false;
        }
        output->push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

bool unhex_string(const std::string &text, std::string *output) {
    std::vector<uint8_t> bytes;
    if (!unhex(text, &bytes)) {
        return false;
    }
    if (bytes.empty()) {
        output->clear();
    } else {
        output->assign(
            reinterpret_cast<const char *>(&bytes[0]),
            bytes.size());
    }
    return true;
}

void append_u32(std::vector<uint8_t> *output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 24));
}

bool read_u32(const std::vector<uint8_t> &input,
              size_t *offset,
              uint32_t *value) {
    if (*offset > input.size() || input.size() - *offset < 4) {
        return false;
    }
    *value =
        static_cast<uint32_t>(input[*offset]) |
        (static_cast<uint32_t>(input[*offset + 1]) << 8) |
        (static_cast<uint32_t>(input[*offset + 2]) << 16) |
        (static_cast<uint32_t>(input[*offset + 3]) << 24);
    *offset += 4;
    return true;
}

bool decode_headers(const std::string &encoded,
                    std::vector<Header> *headers) {
    std::vector<uint8_t> bytes;
    if (!unhex(encoded, &bytes)) {
        return false;
    }
    size_t offset = 0;
    uint32_t count = 0;
    if (!read_u32(bytes, &offset, &count) || count > 65535u) {
        return false;
    }
    headers->clear();
    headers->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t name_size = 0;
        uint32_t value_size = 0;
        if (!read_u32(bytes, &offset, &name_size) ||
            offset > bytes.size() ||
            name_size > bytes.size() - offset) {
            return false;
        }
        const std::string name(
            reinterpret_cast<const char *>(&bytes[offset]),
            name_size);
        offset += name_size;
        if (!read_u32(bytes, &offset, &value_size) ||
            offset > bytes.size() ||
            value_size > bytes.size() - offset) {
            return false;
        }
        const std::string value =
            value_size == 0
                ? std::string()
                : std::string(
                      reinterpret_cast<const char *>(&bytes[offset]),
                      value_size);
        offset += value_size;
        headers->push_back(Header(name, value));
    }
    return offset == bytes.size();
}

std::string encode_headers(const std::vector<Header> &headers) {
    std::vector<uint8_t> bytes;
    append_u32(&bytes, static_cast<uint32_t>(headers.size()));
    for (size_t index = 0; index < headers.size(); ++index) {
        append_u32(
            &bytes, static_cast<uint32_t>(headers[index].first.size()));
        bytes.insert(
            bytes.end(),
            headers[index].first.begin(),
            headers[index].first.end());
        append_u32(
            &bytes, static_cast<uint32_t>(headers[index].second.size()));
        bytes.insert(
            bytes.end(),
            headers[index].second.begin(),
            headers[index].second.end());
    }
    return hex(&bytes[0], bytes.size());
}

std::vector<std::string> split(const std::string &line) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) {
        fields.push_back(field);
    }
    return fields;
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::string();
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void emit_fatal(const std::string &message) {
    std::cout << "FATAL " << hex(message) << std::endl;
}

bool acceptable_flush(capsid_result result) {
    return result == CAPSID_OK || result == CAPSID_WOULD_BLOCK;
}

void poll_worker(capsid_worker *worker, capsid_result flush_result) {
    capsid_pollfd descriptor = {};
    descriptor.fd = capsid_worker_fd(worker);
    descriptor.events =
        POLLIN | (flush_result == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
    capsid::win32::capsid_poll(&descriptor, 1, 10);
}

bool wait_ready(capsid_worker *worker, std::string *error) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *error = std::string("startup flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result =
                capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *error = std::string("startup event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_READY) {
                return true;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                error->assign(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                return false;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *error = "worker exited before READY";
                return false;
            }
        }
        poll_worker(worker, flush);
    }
    *error = "worker READY timeout";
    return false;
}

bool copy_response_headers(const capsid_event &event,
                           std::vector<Header> *headers,
                           std::string *error) {
    size_t count = 0;
    const capsid_result count_result =
        capsid_response_header_count(&event, &count);
    if (count_result != CAPSID_OK) {
        *error = std::string("response header count: ") +
            capsid_result_string(count_result);
        return false;
    }
    headers->clear();
    headers->reserve(count);
    for (size_t index = 0; index < count; ++index) {
        capsid_header header = {};
        const capsid_result result =
            capsid_response_header_at(&event, index, &header);
        if (result != CAPSID_OK) {
            *error = std::string("response header: ") +
                capsid_result_string(result);
            return false;
        }
        headers->push_back(Header(
            std::string(
                reinterpret_cast<const char *>(header.name.data),
                header.name.size),
            std::string(
                reinterpret_cast<const char *>(header.value.data),
                header.value.size)));
    }
    return true;
}

bool run_request(capsid_worker *worker,
                 uint64_t request_id,
                 const std::string &method,
                 const std::string &url,
                 const std::vector<Header> &headers,
                 const std::vector<uint8_t> &body,
                 size_t requested_chunk_size,
                 Response *response,
                 NativeEvents *events,
                 std::string *fatal) {
    std::vector<capsid_header> native_headers(headers.size());
    for (size_t index = 0; index < headers.size(); ++index) {
        native_headers[index].name.data =
            reinterpret_cast<const uint8_t *>(headers[index].first.data());
        native_headers[index].name.size = headers[index].first.size();
        native_headers[index].value.data =
            reinterpret_cast<const uint8_t *>(headers[index].second.data());
        native_headers[index].value.size = headers[index].second.size();
    }
    capsid_result result = capsid_worker_begin_request(
        worker,
        request_id,
        method.c_str(),
        url.c_str(),
        native_headers.empty() ? NULL : &native_headers[0],
        native_headers.size());
    if (result != CAPSID_OK) {
        *fatal = std::string("begin request: ") + capsid_result_string(result);
        return false;
    }

    size_t body_offset = 0;
    uint64_t request_credit = 0;
    bool request_ended = false;
    bool received_head = false;
    const size_t chunk_size =
        requested_chunk_size == 0 ? 1 : requested_chunk_size;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (std::chrono::steady_clock::now() < deadline) {
        while (body_offset < body.size() && request_credit != 0) {
            const size_t remaining = body.size() - body_offset;
            const size_t count = static_cast<size_t>(
                std::min<uint64_t>(
                    std::min(remaining, chunk_size),
                    request_credit));
            result = capsid_worker_write_request(
                worker, request_id, &body[body_offset], count);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("write request: ") +
                    capsid_result_string(result);
                return false;
            }
            body_offset += count;
            request_credit -= count;
        }
        if (!request_ended && body_offset == body.size()) {
            result = capsid_worker_end_request(worker, request_id);
            if (result == CAPSID_OK) {
                request_ended = true;
            } else if (result != CAPSID_WOULD_BLOCK) {
                *fatal = std::string("end request: ") +
                    capsid_result_string(result);
                return false;
            }
        }

        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("request flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("request event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                event.request_id == request_id) {
                request_credit += event.credit;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
                event.request_id == request_id) {
                if (received_head) {
                    *fatal = "duplicate response head";
                    return false;
                }
                received_head = true;
                response->status = event.status;
                capsid_bytes status_text = {};
                if (capsid_response_status_text(
                        &event, &status_text) != CAPSID_OK) {
                    *fatal = "response status text";
                    return false;
                }
                if (status_text.size == 0) {
                    response->status_text.clear();
                } else {
                    response->status_text.assign(
                        reinterpret_cast<const char *>(status_text.data),
                        status_text.size);
                }
                if (!copy_response_headers(
                        event, &response->headers, fatal)) {
                    return false;
                }
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == request_id) {
                if (!received_head) {
                    *fatal = "response body arrived before head";
                    return false;
                }
                if (event.payload.size != 0) {
                    response->body.insert(
                        response->body.end(),
                        event.payload.data,
                        event.payload.data + event.payload.size);
                }
                if (event.payload.size != 0) {
                    result = capsid_worker_grant_response_credit(
                        worker,
                        request_id,
                        static_cast<uint32_t>(event.payload.size));
                    if (result != CAPSID_OK) {
                        *fatal = std::string("response credit: ") +
                            capsid_result_string(result);
                        return false;
                    }
                }
                continue;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END &&
                event.request_id == request_id) {
                if (!received_head) {
                    *fatal = "response ended before head";
                    return false;
                }
                return true;
            }
            if ((event.type == CAPSID_EVENT_ERROR ||
                 event.type == CAPSID_EVENT_REQUEST_TIMEOUT) &&
                (event.request_id == request_id ||
                 event.request_id == 0)) {
                response->error =
                    event.type == CAPSID_EVENT_REQUEST_TIMEOUT
                        ? "TimeoutError: "
                        : "RuntimeError: ";
                response->error.append(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                return true;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited during request";
                return false;
            }
        }
        poll_worker(worker, flush);
    }
    *fatal = "request timeout in framework test driver";
    return false;
}

bool run_cancel(capsid_worker *worker,
                uint64_t request_id,
                const std::string &url,
                const std::string &mode,
                NativeEvents *events,
                std::string *fatal) {
    capsid_result result = capsid_worker_begin_request(
        worker, request_id, "GET", url.c_str(), NULL, 0);
    if (result != CAPSID_OK) {
        *fatal = std::string("begin cancel request: ") +
            capsid_result_string(result);
        return false;
    }
    result = capsid_worker_end_request(worker, request_id);
    if (result != CAPSID_OK) {
        *fatal = std::string("end cancel request: ") +
            capsid_result_string(result);
        return false;
    }

    bool trigger_seen = false;
    std::chrono::steady_clock::time_point trigger_time;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("cancel flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.request_id == request_id &&
                mode == "started" &&
                event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                !trigger_seen) {
                trigger_seen = true;
                trigger_time =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(20);
            }
            if (event.request_id == request_id &&
                mode == "body" &&
                event.type == CAPSID_EVENT_RESPONSE_BODY) {
                trigger_seen = true;
                trigger_time = std::chrono::steady_clock::now();
            }
            if ((event.type == CAPSID_EVENT_ERROR ||
                 event.type == CAPSID_EVENT_REQUEST_TIMEOUT) &&
                event.request_id == request_id) {
                *fatal = "request failed before cancellation trigger";
                return false;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited before cancellation";
                return false;
            }
        }
        if (trigger_seen &&
            std::chrono::steady_clock::now() >= trigger_time) {
            result = capsid_worker_cancel(worker, request_id);
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel request: ") +
                    capsid_result_string(result);
                return false;
            }
            break;
        }
        poll_worker(worker, flush);
    }
    if (!trigger_seen) {
        *fatal = "cancellation trigger timeout";
        return false;
    }

    const std::chrono::steady_clock::time_point settle_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("cancel settle flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel settle event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited after cancellation";
                return false;
            }
        }
        poll_worker(worker, flush);
    }
    return true;
}

bool run_cancel_upload(capsid_worker *worker,
                       uint64_t request_id,
                       const std::string &url,
                       NativeEvents *events,
                       std::string *fatal) {
    capsid_result result = capsid_worker_begin_request(
        worker, request_id, "POST", url.c_str(), NULL, 0);
    if (result != CAPSID_OK) {
        *fatal = std::string("begin upload cancellation request: ") +
            capsid_result_string(result);
        return false;
    }

    bool body_written = false;
    bool cancellation_sent = false;
    std::chrono::steady_clock::time_point cancel_time;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("upload cancellation flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("upload cancellation event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.request_id == request_id &&
                event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                !body_written) {
                const size_t body_size = static_cast<size_t>(
                    std::min<uint32_t>(event.credit, 256u));
                if (body_size == 0) {
                    *fatal = "upload cancellation received zero credit";
                    return false;
                }
                std::vector<uint8_t> body(body_size, 0x61);
                result = capsid_worker_write_request(
                    worker, request_id, &body[0], body.size());
                if (result != CAPSID_OK) {
                    *fatal = std::string("upload cancellation write: ") +
                        capsid_result_string(result);
                    return false;
                }
                body_written = true;
                // Cancel before consuming the replenishment credit so the
                // client must tolerate that legitimate late frame.
                cancel_time =
                    std::chrono::steady_clock::now();
                continue;
            }
            if ((event.type == CAPSID_EVENT_ERROR ||
                 event.type == CAPSID_EVENT_REQUEST_TIMEOUT) &&
                event.request_id == request_id) {
                *fatal = "upload request failed before cancellation";
                return false;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END &&
                event.request_id == request_id) {
                *fatal = "upload request completed before cancellation";
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited before upload cancellation";
                return false;
            }
        }
        if (body_written &&
            std::chrono::steady_clock::now() >= cancel_time) {
            result = capsid_worker_cancel(worker, request_id);
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel upload request: ") +
                    capsid_result_string(result);
                return false;
            }
            cancellation_sent = true;
            break;
        }
        poll_worker(worker, flush);
    }
    if (!cancellation_sent) {
        *fatal = "upload cancellation trigger timeout";
        return false;
    }

    const std::chrono::steady_clock::time_point settle_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("upload cancellation settle flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("upload cancellation settle event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited after upload cancellation";
                return false;
            }
        }
        poll_worker(worker, flush);
    }
    return true;
}

// Cancels a request whose handler schedules a detached native continuation
// (identified by the `marker` log message). After the cancel the worker
// must never run that continuation: a LOG carrying the marker during the
// settle window is a fatal failure (the pre-poison bridge runs it with
// request_id 0). The fixed implementation is expected to poison the worker
// instead, which is reported back as `exited`.
bool run_cancel_continuation(capsid_worker *worker,
                             uint64_t request_id,
                             const std::string &url,
                             const std::string &marker,
                             NativeEvents *events,
                             bool *exited,
                             std::string *fatal) {
    capsid_result result = capsid_worker_begin_request(
        worker, request_id, "GET", url.c_str(), NULL, 0);
    if (result != CAPSID_OK) {
        *fatal = std::string("begin cancel-continuation request: ") +
            capsid_result_string(result);
        return false;
    }
    result = capsid_worker_end_request(worker, request_id);
    if (result != CAPSID_OK) {
        *fatal = std::string("end cancel-continuation request: ") +
            capsid_result_string(result);
        return false;
    }

    bool trigger_seen = false;
    std::chrono::steady_clock::time_point trigger_time;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("cancel-continuation flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel-continuation event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
                continue;
            }
            if (event.request_id == request_id &&
                event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                !trigger_seen) {
                trigger_seen = true;
                trigger_time =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(20);
            }
            if ((event.type == CAPSID_EVENT_ERROR ||
                 event.type == CAPSID_EVENT_REQUEST_TIMEOUT) &&
                event.request_id == request_id) {
                *fatal = "request failed before cancel-continuation trigger";
                return false;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited before cancel-continuation trigger";
                return false;
            }
        }
        if (trigger_seen &&
            std::chrono::steady_clock::now() >= trigger_time) {
            result = capsid_worker_cancel(worker, request_id);
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel continuation request: ") +
                    capsid_result_string(result);
                return false;
            }
            break;
        }
        poll_worker(worker, flush);
    }
    if (!trigger_seen) {
        *fatal = "cancel-continuation trigger timeout";
        return false;
    }

    // The detached continuation is scheduled 80ms after the handler ran;
    // settle long enough for it to fire on the broken implementation.
    *exited = false;
    const std::chrono::steady_clock::time_point settle_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("cancel-continuation settle flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("cancel-continuation settle event: ") +
                    capsid_result_string(result);
                return false;
            }
            if (event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                if (event.type == CAPSID_EVENT_LOG &&
                    log_message(event) == marker) {
                    *fatal = "terminal continuation produced native event '" +
                        marker + "' with request_id " +
                        std::to_string(event.request_id);
                    return false;
                }
                collect_native_event(event, events);
                continue;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                *exited = true;
                return true;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                // Terminalization of the canceled request is expected;
                // keep settling for the poison EXIT.
                continue;
            }
        }
        poll_worker(worker, flush);
    }
    // The continuation never ran but the worker also did not poison
    // itself. The test decides whether that satisfies the contract.
    return true;
}

bool run_concurrent(capsid_worker *worker,
                    uint64_t first_id,
                    const std::string &first_url,
                    uint64_t second_id,
                    const std::string &second_url,
                    Response *first,
                    Response *second,
                    NativeEvents *events,
                    std::string *fatal) {
    const uint64_t ids[] = { first_id, second_id };
    const std::string urls[] = { first_url, second_url };
    Response *responses[] = { first, second };
    bool ended[] = { false, false };
    for (size_t index = 0; index < 2; ++index) {
        capsid_result result = capsid_worker_begin_request(
            worker,
            ids[index],
            "GET",
            urls[index].c_str(),
            NULL,
            0);
        if (result != CAPSID_OK) {
            *fatal = std::string("begin concurrent request: ") +
                capsid_result_string(result);
            return false;
        }
        result = capsid_worker_end_request(worker, ids[index]);
        if (result != CAPSID_OK) {
            *fatal = std::string("end concurrent request: ") +
                capsid_result_string(result);
            return false;
        }
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (!acceptable_flush(flush)) {
            *fatal = std::string("concurrent flush: ") +
                capsid_result_string(flush);
            return false;
        }
        for (;;) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result =
                capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            if (result != CAPSID_OK) {
                *fatal = std::string("concurrent event: ") +
                    capsid_result_string(result);
                return false;
            }
            size_t index = 2;
            if (event.request_id == first_id) {
                index = 0;
            } else if (event.request_id == second_id) {
                index = 1;
            }
            if (index < 2 && event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                responses[index]->status = event.status;
                capsid_bytes status_text = {};
                if (capsid_response_status_text(
                        &event, &status_text) != CAPSID_OK) {
                    *fatal = "concurrent response status text";
                    return false;
                }
                if (status_text.size == 0) {
                    responses[index]->status_text.clear();
                } else {
                    responses[index]->status_text.assign(
                        reinterpret_cast<const char *>(status_text.data),
                        status_text.size);
                }
                if (!copy_response_headers(
                        event, &responses[index]->headers, fatal)) {
                    return false;
                }
            } else if (
                index < 2 &&
                event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (event.payload.size != 0) {
                    responses[index]->body.insert(
                        responses[index]->body.end(),
                        event.payload.data,
                        event.payload.data + event.payload.size);
                    const capsid_result credit =
                        capsid_worker_grant_response_credit(
                            worker,
                            ids[index],
                            static_cast<uint32_t>(event.payload.size));
                    if (credit != CAPSID_OK) {
                        *fatal = std::string("concurrent credit: ") +
                            capsid_result_string(credit);
                        return false;
                    }
                }
            } else if (
                index < 2 &&
                event.type == CAPSID_EVENT_RESPONSE_END) {
                ended[index] = true;
            } else if (
                event.type == CAPSID_EVENT_LOG ||
                event.type == CAPSID_EVENT_AUDIT) {
                collect_native_event(event, events);
            } else if (
                event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                *fatal = "concurrent framework request failed";
                return false;
            } else if (event.type == CAPSID_EVENT_EXIT) {
                *fatal = "worker exited during concurrent requests";
                return false;
            }
        }
        if (ended[0] && ended[1]) {
            return true;
        }
        poll_worker(worker, flush);
    }
    *fatal = "concurrent framework request timeout";
    return false;
}

void emit_result(uint64_t request_id, const Response &response) {
    std::cout
        << "RESULT " << request_id
        << " " << response.status
        << " " << encode_headers(response.headers)
        << " " << hex(
            response.body.empty() ? NULL : &response.body[0],
            response.body.size())
        << " " << hex(response.error)
        << " " << hex(response.status_text)
        << std::endl;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3 || (argc % 2) == 0) {
        emit_fatal(
            "expected worker path, framework bundle path and optional flag pairs");
        return 1;
    }
    const std::string bundle = read_file(argv[2]);
    if (bundle.empty()) {
        emit_fatal("cannot read framework bundle");
        return 1;
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;
    config.initial_stream_window = 1024;
    config.request_timeout_ms = 5000;

    capsid_egress_rule loopback_rule;
    capsid_egress_rule_init(&loopback_rule);
    capsid_egress_policy egress_policy;
    capsid_egress_policy_init(&egress_policy);
    for (int index = 3; index < argc; index += 2) {
        char *end = NULL;
        const unsigned long parsed =
            std::strtoul(argv[index + 1], &end, 10);
        if (!end || *end != '\0') {
            emit_fatal("invalid framework driver option value");
            return 1;
        }
        const std::string option(argv[index]);
        if (option == "--timeout-ms" && parsed != 0) {
            config.request_timeout_ms = parsed;
        } else if (option == "--collect-events") {
            collect_events = parsed != 0;
        } else if (
            option == "--loopback-port" &&
            parsed != 0 &&
            parsed <= 65535u) {
            loopback_rule.action = CAPSID_EGRESS_ALLOW;
            loopback_rule.target = "127.0.0.1";
            loopback_rule.port_start = static_cast<uint16_t>(parsed);
            loopback_rule.port_end = static_cast<uint16_t>(parsed);
            loopback_rule.rule_id = 1;
            egress_policy.default_action = CAPSID_EGRESS_DENY;
            egress_policy.rules = &loopback_rule;
            egress_policy.rule_count = 1;
            config.egress_policy = &egress_policy;
        } else {
            emit_fatal("unknown or invalid framework driver option");
            return 1;
        }
    }

    capsid_worker *worker = NULL;
    capsid_result result = capsid_worker_spawn(&config, &worker);
    if (result != CAPSID_OK) {
        emit_fatal(
            std::string("spawn worker: ") + capsid_result_string(result));
        return 1;
    }
    result = capsid_worker_load_bundle_named(
        worker,
        reinterpret_cast<const uint8_t *>(bundle.data()),
        bundle.size(),
        "https://compat.example/framework-reference.js");
    if (result != CAPSID_OK) {
        emit_fatal(
            std::string("load framework bundle: ") + capsid_result_string(result));
        capsid_worker_destroy(worker);
        return 1;
    }
    std::string error;
    if (!wait_ready(worker, &error)) {
        emit_fatal(error);
        capsid_worker_destroy(worker);
        return 1;
    }
    std::cout << "READY" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        const std::vector<std::string> fields = split(line);
        if (fields.size() == 1 && fields[0] == "STOP") {
            capsid_worker_destroy(worker);
            return 0;
        }
        if (fields.size() == 4 && fields[0] == "CANCEL_CONTINUATION") {
            char *end = NULL;
            const unsigned long long parsed_id =
                std::strtoull(fields[1].c_str(), &end, 10);
            std::string url;
            std::string marker;
            if (!end || *end != '\0' || parsed_id == 0 ||
                !unhex_string(fields[2], &url) ||
                !unhex_string(fields[3], &marker)) {
                emit_fatal("invalid framework cancel-continuation command");
                capsid_worker_destroy(worker);
                return 1;
            }
            NativeEvents events;
            bool exited = false;
            std::string fatal;
            if (!run_cancel_continuation(
                    worker,
                    static_cast<uint64_t>(parsed_id),
                    url,
                    marker,
                    &events,
                    &exited,
                    &fatal)) {
                emit_fatal(fatal);
                capsid_worker_destroy(worker);
                return 1;
            }
            std::cout << "CANCELED " << parsed_id << std::endl;
            if (exited) {
                std::cout << "EXITED " << parsed_id << std::endl;
            }
            emit_events_block(static_cast<uint64_t>(parsed_id), events);
            continue;
        }
        if (fields.size() == 4 && fields[0] == "CANCEL") {
            char *end = NULL;
            const unsigned long long parsed_id =
                std::strtoull(fields[1].c_str(), &end, 10);
            std::string url;
            if (!end || *end != '\0' || parsed_id == 0 ||
                !unhex_string(fields[2], &url) ||
                (fields[3] != "started" && fields[3] != "body")) {
                emit_fatal("invalid framework cancel command");
                capsid_worker_destroy(worker);
                return 1;
            }
            NativeEvents events;
            std::string fatal;
            if (!run_cancel(
                    worker,
                    static_cast<uint64_t>(parsed_id),
                    url,
                    fields[3],
                    &events,
                    &fatal)) {
                emit_fatal(fatal);
                capsid_worker_destroy(worker);
                return 1;
            }
            std::cout << "CANCELED " << parsed_id << std::endl;
            emit_events_block(static_cast<uint64_t>(parsed_id), events);
            continue;
        }
        if (fields.size() == 3 && fields[0] == "CANCEL_UPLOAD") {
            char *end = NULL;
            const unsigned long long parsed_id =
                std::strtoull(fields[1].c_str(), &end, 10);
            std::string url;
            if (!end || *end != '\0' || parsed_id == 0 ||
                !unhex_string(fields[2], &url)) {
                emit_fatal("invalid framework upload cancellation command");
                capsid_worker_destroy(worker);
                return 1;
            }
            NativeEvents events;
            std::string fatal;
            if (!run_cancel_upload(
                    worker,
                    static_cast<uint64_t>(parsed_id),
                    url,
                    &events,
                    &fatal)) {
                emit_fatal(fatal);
                capsid_worker_destroy(worker);
                return 1;
            }
            std::cout << "CANCELED_UPLOAD " << parsed_id << std::endl;
            emit_events_block(static_cast<uint64_t>(parsed_id), events);
            continue;
        }
        if (fields.size() == 5 && fields[0] == "CONCURRENT") {
            char *first_end = NULL;
            char *second_end = NULL;
            const unsigned long long first_id =
                std::strtoull(fields[1].c_str(), &first_end, 10);
            const unsigned long long second_id =
                std::strtoull(fields[3].c_str(), &second_end, 10);
            std::string first_url;
            std::string second_url;
            if (!first_end || *first_end != '\0' ||
                !second_end || *second_end != '\0' ||
                first_id == 0 || second_id == 0 ||
                first_id == second_id ||
                !unhex_string(fields[2], &first_url) ||
                !unhex_string(fields[4], &second_url)) {
                emit_fatal("invalid framework concurrent command");
                capsid_worker_destroy(worker);
                return 1;
            }
            Response first;
            Response second;
            NativeEvents events;
            std::string fatal;
            if (!run_concurrent(
                    worker,
                    static_cast<uint64_t>(first_id),
                    first_url,
                    static_cast<uint64_t>(second_id),
                    second_url,
                    &first,
                    &second,
                    &events,
                    &fatal)) {
                emit_fatal(fatal);
                capsid_worker_destroy(worker);
                return 1;
            }
            emit_result(static_cast<uint64_t>(first_id), first);
            emit_result(static_cast<uint64_t>(second_id), second);
            emit_events_block(static_cast<uint64_t>(first_id), events);
            emit_events_block(static_cast<uint64_t>(second_id), events);
            continue;
        }
        if (fields.size() != 7 || fields[0] != "REQUEST") {
            emit_fatal("invalid framework driver command");
            capsid_worker_destroy(worker);
            return 1;
        }

        char *end = NULL;
        const unsigned long long parsed_id =
            std::strtoull(fields[1].c_str(), &end, 10);
        if (!end || *end != '\0' || parsed_id == 0) {
            emit_fatal("invalid request id");
            capsid_worker_destroy(worker);
            return 1;
        }
        end = NULL;
        const unsigned long parsed_chunk_size =
            std::strtoul(fields[6].c_str(), &end, 10);
        if (!end || *end != '\0' || parsed_chunk_size == 0) {
            emit_fatal("invalid request chunk size");
            capsid_worker_destroy(worker);
            return 1;
        }

        std::string method;
        std::string url;
        std::vector<Header> headers;
        std::vector<uint8_t> body;
        if (!unhex_string(fields[2], &method) ||
            !unhex_string(fields[3], &url) ||
            !decode_headers(fields[4], &headers) ||
            !unhex(fields[5], &body)) {
            emit_fatal("invalid encoded framework request");
            capsid_worker_destroy(worker);
            return 1;
        }

        Response response;
        NativeEvents events;
        std::string fatal;
        if (!run_request(
                worker,
                static_cast<uint64_t>(parsed_id),
                method,
                url,
                headers,
                body,
                static_cast<size_t>(parsed_chunk_size),
                &response,
                &events,
                &fatal)) {
            emit_fatal(fatal);
            capsid_worker_destroy(worker);
            return 1;
        }
        emit_result(static_cast<uint64_t>(parsed_id), response);
        emit_events_block(static_cast<uint64_t>(parsed_id), events);
    }

    capsid_worker_destroy(worker);
    return 0;
}
