#include "capsid/runtime.h"
#include "egress_test_policy.h"
#include "graceful_worker_exit.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <arpa/inet.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <netinet/in.h>
#endif
#include "win32_compat.h"
#include <signal.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/socket.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/wait.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

void require_clean_worker_shutdown(
    capsid_worker *worker,
    const char *context) {
    const capsid_test::GracefulWorkerExit result =
        capsid_test::shutdown_and_wait(worker, 2000);
    capsid_worker_destroy(worker);

    if (result.shutdown_result != CAPSID_OK) {
        fail(std::string(context) + " could not queue graceful shutdown: " +
             capsid_result_string(result.shutdown_result));
    }
    if (result.flush_result != CAPSID_OK) {
        fail(std::string(context) + " could not flush graceful shutdown: " +
             capsid_result_string(result.flush_result));
    }
    if (result.wait_error != 0) {
        fail(std::string(context) + " waitpid failed: " +
             std::strerror(result.wait_error));
    }
    if (!result.reaped) {
        fail(std::string(context) +
             " did not exit within the graceful shutdown deadline");
    }
    if (WIFSIGNALED(result.status)) {
        fail(std::string(context) + " terminated by signal " +
             std::to_string(WTERMSIG(result.status)));
    }
    if (!WIFEXITED(result.status) || WEXITSTATUS(result.status) != 0) {
        fail(std::string(context) + " exited abnormally with status " +
             std::to_string(result.status));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open TLS fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

uint16_t unused_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail("cannot create TLS port socket");
    }
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<const struct sockaddr *>(&address),
             sizeof(address)) != 0) {
        close(fd);
        fail("cannot bind TLS port socket");
    }
    socklen_t size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&address), &size) != 0) {
        close(fd);
        fail("cannot resolve TLS test port");
    }
    const uint16_t port = ntohs(address.sin_port);
    close(fd);
    return port;
}

class OpenSslRsaPssServer {
public:
    OpenSslRsaPssServer(const char *openssl_path,
                        const char *certificate,
                        const char *private_key,
                        uint16_t port)
        : pid_(-1) {
        const std::string port_text = std::to_string(port);
        pid_ = fork();
        if (pid_ == 0) {
            execl(openssl_path,
                  openssl_path,
                  "s_server",
                  "-accept",
                  port_text.c_str(),
                  "-cert",
                  certificate,
                  "-key",
                  private_key,
                  "-tls1_2",
                  "-sigalgs",
                  "rsa_pss_rsae_sha256",
                  "-www",
                  "-quiet",
                  "-no_ign_eof",
                  static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid_ < 0) {
            fail(std::string("cannot launch OpenSSL TLS test server: ") +
                 std::strerror(errno));
        }

        for (int attempt = 0; attempt < 100; ++attempt) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                fail(std::string("OpenSSL TLS test server exited early: ") +
                     std::to_string(status));
            }

            const int probe = socket(AF_INET, SOCK_STREAM, 0);
            if (probe >= 0) {
                struct sockaddr_in address = {};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port);
                const int connected = connect(
                    probe,
                    reinterpret_cast<const struct sockaddr *>(&address),
                    sizeof(address));
                close(probe);
                if (connected == 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50));
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        stop();
        fail("OpenSSL TLS test server did not start listening");
    }

    ~OpenSslRsaPssServer() { stop(); }

private:
    void stop() {
        if (pid_ <= 0) {
            return;
        }
        kill(pid_, SIGTERM);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

    pid_t pid_;
};

class MbedTlsServer {
public:
    MbedTlsServer(const char *certificate,
                  const char *private_key,
                  uint16_t port)
        : stopping_(false) {
        mbedtls_net_init(&listener_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_x509_crt_init(&certificate_);
        mbedtls_pk_init(&private_key_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&random_);

        static const unsigned char personalization[] =
            "capsid-runtime-direct-fetch-tls";
        int result = mbedtls_ctr_drbg_seed(
            &random_,
            mbedtls_entropy_func,
            &entropy_,
            personalization,
            sizeof(personalization) - 1);
        if (result == 0) {
            result = mbedtls_x509_crt_parse_file(
                &certificate_, certificate);
        }
        if (result == 0) {
            result = mbedtls_pk_parse_keyfile(
                &private_key_,
                private_key,
                NULL,
                mbedtls_ctr_drbg_random,
                &random_);
        }
        if (result == 0) {
            result = mbedtls_ssl_config_defaults(
                &config_,
                MBEDTLS_SSL_IS_SERVER,
                MBEDTLS_SSL_TRANSPORT_STREAM,
                MBEDTLS_SSL_PRESET_DEFAULT);
        }
        if (result == 0) {
            mbedtls_ssl_conf_rng(
                &config_, mbedtls_ctr_drbg_random, &random_);
            result = mbedtls_ssl_conf_own_cert(
                &config_, &certificate_, &private_key_);
        }
        const std::string port_text = std::to_string(port);
        if (result == 0) {
            result = mbedtls_net_bind(
                &listener_,
                "127.0.0.1",
                port_text.c_str(),
                MBEDTLS_NET_PROTO_TCP);
        }
        if (result != 0) {
            fail(std::string("cannot initialize mbedTLS test server: ") +
                 std::to_string(result));
        }
        thread_ = std::thread(&MbedTlsServer::serve, this);
    }

    ~MbedTlsServer() {
        stopping_ = true;
        if (listener_.fd >= 0) {
            shutdown(listener_.fd, SHUT_RDWR);
        }
        mbedtls_net_free(&listener_);
        if (thread_.joinable()) {
            thread_.join();
        }
        mbedtls_ssl_config_free(&config_);
        mbedtls_x509_crt_free(&certificate_);
        mbedtls_pk_free(&private_key_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_entropy_free(&entropy_);
    }

private:
    static bool retryable(int result) {
        return result == MBEDTLS_ERR_SSL_WANT_READ ||
               result == MBEDTLS_ERR_SSL_WANT_WRITE;
    }

    void serve_client(mbedtls_net_context *client) {
        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        int result = mbedtls_ssl_setup(&ssl, &config_);
        if (result == 0) {
            mbedtls_ssl_set_bio(
                &ssl,
                client,
                mbedtls_net_send,
                mbedtls_net_recv,
                NULL);
            do {
                result = mbedtls_ssl_handshake(&ssl);
            } while (retryable(result));
        }
        if (result == 0) {
            char request[4096];
            size_t used = 0;
            while (used < sizeof(request) &&
                   (used < 4 ||
                    std::string(request, used).find("\r\n\r\n") ==
                        std::string::npos)) {
                result = mbedtls_ssl_read(
                    &ssl,
                    reinterpret_cast<unsigned char *>(request) + used,
                    sizeof(request) - used);
                if (result > 0) {
                    used += static_cast<size_t>(result);
                    continue;
                }
                if (!retryable(result)) {
                    break;
                }
            }
            if (used > 0) {
                static const char response[] =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 13\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "capsid tls ok";
                size_t offset = 0;
                while (offset < sizeof(response) - 1) {
                    result = mbedtls_ssl_write(
                        &ssl,
                        reinterpret_cast<const unsigned char *>(response) +
                            offset,
                        sizeof(response) - 1 - offset);
                    if (result > 0) {
                        offset += static_cast<size_t>(result);
                    } else if (!retryable(result)) {
                        break;
                    }
                }
            }
        }
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
    }

    void serve() {
        while (!stopping_) {
            mbedtls_net_context client;
            mbedtls_net_init(&client);
            const int result =
                mbedtls_net_accept(&listener_, &client, NULL, 0, NULL);
            if (result == 0) {
                serve_client(&client);
            }
            mbedtls_net_free(&client);
            if (result != 0 && !stopping_) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
        }
    }

    std::atomic<bool> stopping_;
    mbedtls_net_context listener_;
    mbedtls_ssl_config config_;
    mbedtls_x509_crt certificate_;
    mbedtls_pk_context private_key_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context random_;
    std::thread thread_;
};

uint32_t wait_for_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("TLS startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return event.flags;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("TLS worker startup error: ") +
                     std::string(
                         reinterpret_cast<const char *>(event.payload.data),
                         event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("TLS worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK ||
            std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for TLS worker READY");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

struct Result {
    uint32_t status;
    std::string body;
};

Result run_request(capsid_worker *worker, uint64_t id, const std::string &url) {
    require_result(
        capsid_worker_begin_request(worker, id, "GET", url.c_str(), NULL, 0),
        "begin TLS request");
    require_result(capsid_worker_end_request(worker, id), "end TLS request");

    Result output = {};
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("TLS request flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT ||
                event.type == CAPSID_EVENT_LOG) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                output.status = event.status;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                output.body.append(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, id,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant TLS response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                return output;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_REQUEST_TIMEOUT ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("TLS worker failed during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK ||
            std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for TLS response");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

Result run_case(const char *worker_path,
                const std::string &bundle,
                const char *ca_bundle,
                uint16_t port,
                bool trusted,
                bool rsa_pss,
                uint64_t id,
                bool strict) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.tls_ca_bundle_path = ca_bundle;
    config.strict_sandbox = strict ? 1 : 0;
    LoopbackEgressPolicy egress_policy;
    egress_policy.attach(&config);

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn TLS worker");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load TLS bundle");
    const uint32_t sandbox_features = wait_for_ready(worker);
    if (strict &&
        (sandbox_features & CAPSID_SANDBOX_FEATURE_STRICT_BASE) !=
            CAPSID_SANDBOX_FEATURE_STRICT_BASE) {
        fail("strict TLS worker did not report mandatory sandbox features");
    }
    const std::string url =
        "https://example.test/direct-fetch-tls?port=" +
        std::to_string(port) + "&trusted=" + (trusted ? "1" : "0") +
        "&rsaPss=" + (rsa_pss ? "1" : "0");
    const Result result = run_request(worker, id, url);
    if (strict) {
        require_clean_worker_shutdown(worker, "strict TLS worker");
    } else {
        capsid_worker_destroy(worker);
    }
    return result;
}

void require_passed(const Result &result, const char *mode) {
    if (result.status != 200 ||
        result.body.find("\"passed\":true") == std::string::npos ||
        result.body.find(std::string("\"mode\":\"") + mode + "\"") ==
            std::string::npos) {
        fail(std::string("TLS ") + mode + " case failed with status " +
             std::to_string(result.status) + ": " + result.body);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 6) {
        fail("expected worker, fixture, certificate, key, CA and options");
    }
    bool strict = false;
    const char *openssl_path = NULL;
    for (int index = 6; index < argc; ++index) {
        const std::string option(argv[index]);
        if (option == "--strict" && !strict) {
            strict = true;
        } else if (option == "--openssl" && !openssl_path &&
                   index + 1 < argc) {
            openssl_path = argv[++index];
        } else {
            fail(std::string("unknown TLS test option: ") + option);
        }
    }
    const uint16_t port = unused_port();
    MbedTlsServer server(argv[3], argv[4], port);
    const std::string bundle = read_file(argv[2]);

    require_passed(
        run_case(argv[1], bundle, NULL, port, false, false, 71, strict),
        "untrusted");
    require_passed(
        run_case(argv[1], bundle, argv[5], port, true, false, 72, strict),
        "trusted");
    if (openssl_path) {
        const uint16_t rsa_pss_port = unused_port();
        OpenSslRsaPssServer rsa_pss_server(
            openssl_path, argv[3], argv[4], rsa_pss_port);
        require_passed(
            run_case(argv[1],
                     bundle,
                     argv[5],
                     rsa_pss_port,
                     true,
                     true,
                     73,
                     strict),
            "rsa-pss");
    }
    return 0;
}
