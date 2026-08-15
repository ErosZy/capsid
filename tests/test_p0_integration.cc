#include "capsid/runtime.h"

#include "win32_compat.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void pump(capsid_worker *worker, int timeout_ms) {
    const capsid_result flush = capsid_worker_flush(worker);
    if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
        fail(std::string("flush: ") + capsid_result_string(flush));
    }
    capsid_pollfd descriptor = {};
    descriptor.fd = capsid_worker_fd(worker);
    descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
    capsid::win32::capsid_poll(&descriptor, 1, timeout_ms);
}

bool next_event(capsid_worker *worker, capsid_event *event) {
    std::memset(event, 0, sizeof(*event));
    event->struct_size = sizeof(*event);
    const capsid_result result = capsid_worker_next_event(worker, event);
    if (result == CAPSID_OK) {
        return true;
    }
    if (result != CAPSID_WOULD_BLOCK) {
        fail(std::string("next event: ") + capsid_result_string(result));
    }
    return false;
}

void wait_for_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR || event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed before READY");
            }
        }
    }
    fail("READY timeout");
}

capsid_worker *spawn_loaded(const char *worker_path,
                          const std::string &bundle,
                          uint32_t window,
                          uint64_t timeout_ms) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.initial_stream_window = window;
    config.request_timeout_ms = timeout_ms;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn");
    require_result(
        capsid_worker_load_bundle(
            worker, reinterpret_cast<const uint8_t *>(bundle.data()), bundle.size()),
        "load bundle");
    wait_for_ready(worker);
    return worker;
}

std::vector<std::string> response_header_values(const capsid_event &event,
                                                const char *wanted_name) {
    size_t count = 0;
    require_result(capsid_response_header_count(&event, &count), "header count");
    std::vector<std::string> values;
    for (size_t index = 0; index < count; ++index) {
        capsid_header header = {};
        require_result(
            capsid_response_header_at(&event, index, &header),
            "header at");
        const std::string name(
            reinterpret_cast<const char *>(header.name.data), header.name.size);
        if (name == wanted_name) {
            values.push_back(std::string(
                reinterpret_cast<const char *>(header.value.data),
                header.value.size));
        }
    }
    return values;
}

void wait_for_response_end(capsid_worker *worker, uint64_t request_id) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == request_id) {
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        request_id,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant response credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == request_id) {
                return;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT ||
                       event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                fail("request failed while waiting for response end");
            }
        }
    }
    fail("response end timeout");
}

void test_streaming_credit_headers_and_cancel(const char *worker_path,
                                              const std::string &bundle) {
    static const uint32_t window = 1024;
    static const size_t request_size = 80 * 1024 + 19;
    static const size_t response_size = 96 * 1024 + 37;

    capsid_worker *worker = spawn_loaded(worker_path, bundle, window, 5000);
    require_result(
        capsid_worker_begin_request(
            worker, 41, "POST", "https://example.test/stream", NULL, 0),
        "begin streaming request");

    std::vector<uint8_t> request(request_size);
    for (size_t index = 0; index < request.size(); ++index) {
        request[index] = static_cast<uint8_t>(index % 251);
    }

    require(
        capsid_worker_write_request(
            worker, 41, &request[0], static_cast<size_t>(window + 1)) ==
            CAPSID_WOULD_BLOCK,
        "request write must wait for advertised credit");

    size_t request_offset = 0;
    std::vector<uint8_t> response;
    bool request_ended = false;
    bool received_head = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                event.request_id == 41) {
                size_t remaining = request.size() - request_offset;
                size_t amount = std::min<size_t>(remaining, event.credit);
                if (amount) {
                    require_result(
                        capsid_worker_write_request(
                            worker,
                            41,
                            &request[request_offset],
                            amount),
                        "write credited request bytes");
                    request_offset += amount;
                }
                if (request_offset == request.size() && !request_ended) {
                    require_result(
                        capsid_worker_end_request(worker, 41),
                        "end streaming request");
                    request_ended = true;
                }
            } else if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
                       event.request_id == 41) {
                require(event.status == 201, "streaming response status");
                const std::vector<std::string> cookies =
                    response_header_values(event, "set-cookie");
                require(cookies.size() == 2, "two Set-Cookie values preserved");
                require(cookies[0] == "first=1; Path=/", "first cookie preserved");
                require(cookies[1] == "second=2; Path=/", "second cookie preserved");
                received_head = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                       event.request_id == 41) {
                require(received_head, "response body follows head");
                response.insert(
                    response.end(),
                    event.payload.data,
                    event.payload.data + event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        41,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant response credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 41) {
                require(response.size() == response_size, "response size");
                for (size_t index = 0; index < response.size(); ++index) {
                    require(
                        response[index] == static_cast<uint8_t>(index % 251),
                        "response byte pattern");
                }
                goto stream_complete;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT) {
                fail(
                    std::string("streaming request failed for id ") +
                    std::to_string(event.request_id) + ": " +
                    std::string(
                        reinterpret_cast<const char *>(event.payload.data),
                        event.payload.size));
            }
        }
    }
    fail("streaming request timeout");

stream_complete:
    require_result(
        capsid_worker_begin_request(
            worker, 42, "GET", "https://example.test/cancel", NULL, 0),
        "begin cancel request");
    require_result(capsid_worker_end_request(worker, 42), "end cancel request");
    require_result(capsid_worker_cancel(worker, 42), "cancel request");
    require_result(capsid_worker_cancel(worker, 42), "repeat cancel");
    require_result(
        capsid_worker_begin_request(
            worker, 42, "GET", "https://example.test/reuse", NULL, 0),
        "reuse canceled request id");
    require_result(capsid_worker_end_request(worker, 42), "end reused request");

    bool reused = false;
    const std::chrono::steady_clock::time_point reuse_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!reused && std::chrono::steady_clock::now() < reuse_deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
                event.request_id == 42) {
                const std::vector<std::string> cookies =
                    response_header_values(event, "set-cookie");
                require(cookies.size() == 2, "reuse cookies preserved");
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                       event.request_id == 42) {
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        42,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant reused response credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 42) {
                reused = true;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT) {
                fail(
                    std::string("cancel/reuse request failed for id ") +
                    std::to_string(event.request_id) + ": " +
                    std::string(
                        reinterpret_cast<const char *>(event.payload.data),
                        event.payload.size));
            }
        }
    }
    require(reused, "canceled request id was reusable");

    require_result(
        capsid_worker_begin_request(
            worker,
            43,
            "POST",
            "https://example.test/cancel-body",
            NULL,
            0),
        "begin body cancel request");
    bool body_credit = false;
    while (!body_credit) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT &&
                event.request_id == 43) {
                std::vector<uint8_t> partial(512, 0x61);
                require_result(
                    capsid_worker_write_request(
                        worker, 43, &partial[0], partial.size()),
                    "write partial body before cancel");
                body_credit = true;
                break;
            }
        }
    }
    require_result(capsid_worker_cancel(worker, 43), "cancel request body");
    require_result(
        capsid_worker_cancel(worker, 43),
        "repeat request body cancel");
    require_result(
        capsid_worker_begin_request(
            worker, 43, "GET", "https://example.test/reuse", NULL, 0),
        "reuse body-canceled id");
    require_result(
        capsid_worker_end_request(worker, 43),
        "end body-canceled reuse");
    wait_for_response_end(worker, 43);

    require_result(
        capsid_worker_begin_request(
            worker,
            44,
            "GET",
            "https://example.test/backpressure",
            NULL,
            0),
        "begin response backpressure request");
    require_result(
        capsid_worker_end_request(worker, 44),
        "end response backpressure request");
    bool response_stalled = false;
    while (!response_stalled) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == 44) {
                require(
                    event.payload.size == window,
                    "response stopped at its credit window");
                response_stalled = true;
                break;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("response failed before backpressure cancellation");
            }
        }
    }
    require_result(
        capsid_worker_cancel(worker, 44),
        "cancel response under backpressure");
    require_result(
        capsid_worker_cancel(worker, 44),
        "repeat response backpressure cancel");
    require_result(
        capsid_worker_begin_request(
            worker, 44, "GET", "https://example.test/reuse", NULL, 0),
        "reuse response-canceled id");
    require_result(
        capsid_worker_end_request(worker, 44),
        "end response-canceled reuse");
    wait_for_response_end(worker, 44);
    capsid_worker_destroy(worker);
}

void test_form_data_content_type(const char *worker_path,
                                 const std::string &bundle) {
    capsid_worker *worker = spawn_loaded(worker_path, bundle, 1024, 5000);
    const std::string name = "content-type";
    const std::string value = "application/json";
    capsid_header header = {};
    header.name.data =
        reinterpret_cast<const uint8_t *>(name.data());
    header.name.size = name.size();
    header.value.data =
        reinterpret_cast<const uint8_t *>(value.data());
    header.value.size = value.size();
    require_result(
        capsid_worker_begin_request(
            worker,
            45,
            "POST",
            "https://example.test/formdata-content-type",
            &header,
            1),
        "begin formData MIME request");
    require_result(
        capsid_worker_end_request(worker, 45),
        "end formData MIME request");

    uint32_t status = 0;
    std::string body;
    bool ended = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ended && std::chrono::steady_clock::now() < deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
                event.request_id == 45) {
                status = event.status;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                       event.request_id == 45) {
                body.append(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        45,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant formData response credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 45) {
                ended = true;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT ||
                       event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
                fail("formData MIME request failed");
            }
        }
    }
    require(ended, "formData MIME response ended");
    require(status == 200, "formData rejects non-form MIME");
    require(
        body ==
            "{\"name\":\"TypeError\",\"bodyUsed\":true,"
            "\"contentType\":\"application/json\"}",
        "Request.clone preserves headers and formData rejection consumes body");
    capsid_worker_destroy(worker);
}

void test_sync_timeout(const char *worker_path, const std::string &bundle) {
    capsid_worker *worker = spawn_loaded(worker_path, bundle, 1024, 100);
    require_result(
        capsid_worker_begin_request(
            worker, 77, "GET", "https://example.test/timeout", NULL, 0),
        "begin timeout request");
    require_result(capsid_worker_end_request(worker, 77), "end timeout request");

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool timed_out = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT &&
                event.request_id == 77) {
                timed_out = true;
                break;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                fail("infinite handler unexpectedly completed");
            }
        }
        if (timed_out) {
            break;
        }
    }
    require(timed_out, "synchronous handler did not time out");
    capsid_worker_destroy(worker);

    worker = spawn_loaded(worker_path, bundle, 1024, 100);
    require_result(
        capsid_worker_begin_request(
            worker, 78, "GET", "https://example.test/reuse", NULL, 0),
        "begin request on replacement worker");
    require_result(
        capsid_worker_end_request(worker, 78),
        "end request on replacement worker");
    bool recovered = false;
    const std::chrono::steady_clock::time_point recovery_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!recovered &&
           std::chrono::steady_clock::now() < recovery_deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == 78) {
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        78,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant recovery response credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 78) {
                recovered = true;
            } else if (event.type == CAPSID_EVENT_ERROR ||
                       event.type == CAPSID_EVENT_EXIT) {
                fail("replacement worker did not serve request");
            }
        }
    }
    require(recovered, "replacement worker did not serve request");

    require_result(
        capsid_worker_begin_request(
            worker,
            79,
            "GET",
            "https://example.test/async-timeout",
            NULL,
            0),
        "begin async timeout request");
    require_result(
        capsid_worker_end_request(worker, 79),
        "end async timeout request");
    bool async_timed_out = false;
    const std::chrono::steady_clock::time_point async_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!async_timed_out &&
           std::chrono::steady_clock::now() < async_deadline) {
        pump(worker, 10);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT &&
                event.request_id == 79) {
                require(
                    capsid_worker_fd(worker) >= 0,
                    "async timeout must not kill the worker");
                async_timed_out = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                fail("pending async handler unexpectedly completed");
            }
        }
    }
    require(async_timed_out, "async handler did not time out");
    capsid_worker_destroy(worker);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    const std::string bundle = read_file(argv[2]);
    test_streaming_credit_headers_and_cancel(argv[1], bundle);
    test_form_data_content_type(argv[1], bundle);
    test_sync_timeout(argv[1], bundle);
    return 0;
}
