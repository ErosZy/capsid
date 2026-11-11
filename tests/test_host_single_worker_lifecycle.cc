// Frozen M2 RED: split the existing single-worker benchmark server into a
// controllable start/request_stop/wait lifecycle without changing its HTTP
// behavior. StaticPoolServer will compose this lifecycle in the next batch.

#include "host/single_worker_server.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/query.hpp>
#include <boost/asio/execution/context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <arpa/inet.h>
#endif
#include <fcntl.h>
#include "win32_compat.h"
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif
// macOS does not define SOCK_CLOEXEC; the IPC pair does not cross exec on
// this test path, so a plain socket type is the portable fallback.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/time.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Only this test executable replaces allocation. The thread-local probe is
// armed immediately around the real adopt_socket call; all other threads
// allocate normally. Pausing here does not add a production scheduling hook.
namespace adoption_allocation_probe {
class Pause {
public:
    explicit Pause(bool record_only = false) : record_only_(record_only) {}
    void enter(std::size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        bytes_ = bytes;
        entered_ = true;
        condition_.notify_all();
        if (!record_only_) condition_.wait(lock, [this] { return released_; });
    }
    bool wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(6), [this] { return entered_; });
    }
    std::size_t bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }
    void release() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    const bool record_only_;
    bool entered_ = false;
    bool released_ = false;
    std::size_t bytes_ = 0;
};
thread_local Pause* next = nullptr;
}  // namespace adoption_allocation_probe

void* operator new(std::size_t bytes) {
    if (auto* pause = std::exchange(adoption_allocation_probe::next, nullptr)) pause->enter(bytes);
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { return std::malloc(bytes ? bytes : 1); }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { return std::malloc(bytes ? bytes : 1); }
// Keep replacement deallocation as a real boundary. GCC otherwise inlines
// it into Boost exception clones and mistakes our matched malloc/free pair
// for a standard-new/free mismatch. No diagnostic is disabled.
#if defined(__GNUC__)
#define CAPSID_TEST_ALLOCATION_BOUNDARY __attribute__((noinline))
#else
#define CAPSID_TEST_ALLOCATION_BOUNDARY
#endif
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void* memory) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void* memory) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void* memory, std::size_t, const std::nothrow_t&) noexcept { std::free(memory); }
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void* memory, std::size_t, const std::nothrow_t&) noexcept { std::free(memory); }
#undef CAPSID_TEST_ALLOCATION_BOUNDARY

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::string read_ready_line(int fd) {
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.find('\n') == std::string::npos) {
        require(std::chrono::steady_clock::now() < deadline,
                "server did not publish READY after start returned");
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
        require(polled >= 0, "cannot poll READY pipe");
        if (polled == 0) {
            continue;
        }
        char bytes[512];
#if defined(_WIN32)
        const ssize_t count =
            capsid::win32::read_fd(fd, bytes, sizeof(bytes));
#else
        const ssize_t count = read(fd, bytes, sizeof(bytes));
#endif
        require(count > 0, "READY pipe closed without a record");
        line.append(bytes, static_cast<std::size_t>(count));
    }
    return line.substr(0, line.find('\n'));
}

std::uint16_t ready_port(const std::string& line) {
    const std::string marker = "\"port\":";
    const std::string::size_type begin = line.find(marker);
    require(begin != std::string::npos, "READY record has no port");
    const char* digits = line.c_str() + begin + marker.size();
    char* end = nullptr;
    const unsigned long port = std::strtoul(digits, &end, 10);
    require(end != digits && port > 0 && port <= 65535,
            "READY record has an invalid port");
    return static_cast<std::uint16_t>(port);
}

void require_http_response(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create lifecycle HTTP socket");
    require(capsid::win32::setsockopt_recv_timeout_fd(fd, 3000) == 0,
            "cannot set lifecycle HTTP timeout");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode loopback address");
    require(capsid::win32::connect_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                    sizeof(address)) == 0,
            "cannot connect after server start");
    const std::string request =
        "GET /@capsid/orders/lifecycle HTTP/1.1\r\n"
        "Host: public.example\r\n"
        "Connection: close\r\n\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count =
            capsid::win32::send_fd(fd, request.data() + sent, request.size() - sent, 0);
        require(count > 0, "cannot write lifecycle HTTP request");
        sent += static_cast<std::size_t>(count);
    }
    std::string response;
    char bytes[2048];
    for (;;) {
        const ssize_t count = capsid::win32::recv_fd(fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            break;
        }
        require(count > 0, "cannot read lifecycle HTTP response");
        response.append(bytes, static_cast<std::size_t>(count));
    }
    close(fd);
    require(response.find(" 200 ") != std::string::npos &&
                response.find("lifecycle-ok") != std::string::npos,
            "started server did not preserve single-worker HTTP behavior");
}

void require_port_closed(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create closed-port probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode closed-port probe address");
    const int connected = capsid::win32::connect_fd(
        fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address));
    close(fd);
    require(connected != 0,
            "destroying a running server left its listener active");
}

std::uint16_t reserve_test_port() {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create port-reservation socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode port-reservation address");
    require(capsid::win32::bind_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                 sizeof(address)) == 0,
            "cannot reserve lifecycle test port");
    socklen_t length = sizeof(address);
    require(capsid::win32::getsockname_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                        &length) == 0,
            "cannot inspect lifecycle test port");
    const std::uint16_t port = ntohs(address.sin_port);
    close(fd);
    require(port != 0, "kernel selected an invalid lifecycle test port");
    return port;
}

void require_port_bindable(std::uint16_t port) {
    const int fd = capsid::win32::create_tcp_socket_fd();
    require(fd >= 0, "cannot create listener-cleanup probe socket");
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
            "cannot encode listener-cleanup probe address");
    const int bound = capsid::win32::bind_fd(fd, reinterpret_cast<struct sockaddr*>(&address),
                           sizeof(address));
    close(fd);
    require(bound == 0,
            "failed start returned while its listener was still bound");
}

capsid::host::SingleWorkerServerOptions make_options(
    const char* worker_path, int ready_fd) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "lifecycle-inline";
    options.source_name = "file://orders/v1/bundle.mjs";
    options.application = "orders";
    options.listen_address = "127.0.0.1";
    options.listen_port = 0;
    options.public_scheme = "http";
    options.public_authority = "public.example";
    options.request_timeout_ms = 5000;
    options.initial_stream_window = 64U * 1024U;
    options.strict_sandbox = false;
    options.ready_fd = ready_fd;
    return options;
}

// The Node driver observes actual child PIDs and controls process exit. Keep
// these barriers in the test fixture: the production server needs no test hook.
class ExternalAcceptExitFixture {
public:
    ExternalAcceptExitFixture(const char* worker, std::string scenario)
        : selected_(external_options(worker)), healthy_(external_options(worker)),
          acceptor_(accept_io_), work_(accept_io_.get_executor()),
          scenario_(std::move(scenario)) {}

    ~ExternalAcceptExitFixture() { stop(); }

    int run() {
        const std::string source =
            "export default {fetch(r) {return new Response('healthy:' + "
            "new URL(r.url).searchParams.get('challenge'));}};";
        const std::vector<std::uint8_t> bundle(source.begin(), source.end());
        std::string error;
        check(selected_.start(bundle, &error), "selected start: " + error);
        event("selected-ready");
        check(command() == "arm", "expected arm command");
        check(healthy_.start(bundle, &error), "healthy start: " + error);
        if (pre_post_scenario()) {
            adoption_allocation_probe::Pause calibration(true);
            const auto executor = selected_.accept_executor();
            adoption_allocation_probe::next = &calibration;
            allocation_calibration_ = std::make_shared<boost::asio::ip::tcp::socket>(executor);
            adoption_allocation_probe::next = nullptr;
            expected_allocation_bytes_ = calibration.bytes();
            check(expected_allocation_bytes_ > 0, "socket allocation calibration did not run");
        }
        if (scenario_ == "queued-before-exit") {
            boost::asio::post(selected_.accept_executor(), [this] {
                std::unique_lock<std::mutex> lock(reactor_mutex_);
                reactor_paused_ = true;
                reactor_condition_.notify_all();
                reactor_condition_.wait(lock, [this] { return reactor_released_; });
            });
            std::unique_lock<std::mutex> lock(reactor_mutex_);
            check(reactor_condition_.wait_for(lock, std::chrono::seconds(6),
                      [this] { return reactor_paused_; }), "reactor pause barrier timed out");
            event("reactor-paused");
        }
        using tcp = boost::asio::ip::tcp;
        acceptor_.open(tcp::v4());
        acceptor_.bind(tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
        acceptor_.listen();
        const auto port = acceptor_.local_endpoint().port();
        // Like StaticPoolServer, select the executor and arm accept before
        // announcing that the listener is ready for a client connection.
        acceptor_.async_accept(selected_.accept_executor(),
            [this](boost::system::error_code ec, tcp::socket peer) {
                ++selected_completions_;
                if (ec) {
                    if (scenario_ == "cancel-pending" &&
                        ec == boost::asio::error::operation_aborted) {
                        ++cancelled_;
                        event("accept-cancelled");
                    } else {
                        fixture_error("unexpected selected accept error");
                    }
                    return;
                }
                if (peer.get_executor() != selected_.accept_executor()) {
                    fixture_error("selected peer has a foreign executor");
                    return;
                }
                if (pre_post_scenario()) {
                    bool accepted = false;
                    try {
                        adoption_allocation_probe::next = &allocation_pause_;
                        accepted = selected_.adopt_socket(std::move(peer));
                        adoption_allocation_probe::next = nullptr;
                    } catch (...) {
                        adoption_allocation_probe::next = nullptr;
                        fixture_error("pre-post adoption threw an exception");
                        return;
                    }
                    if (accepted) ++queued_adoptions_;
                    else ++refused_;
                    event("adoption-returned", accepted ? ",\"accepted\":true" : ",\"accepted\":false");
                    accept_healthy_successor();
                } else if (adopted_scenario()) {
                    if (!selected_.adopt_socket(std::move(peer))) {
                        fixture_error("live Worker refused queued adoption");
                        return;
                    }
                    ++queued_adoptions_;
                    // This marker follows the real adoption handler on its
                    // owner executor, and is posted before the driver may
                    // exit the Worker. Assert that no earlier event pump
                    // silently turns this into a pre-adoption refusal case.
                    boost::asio::post(selected_.accept_executor(), [this] {
                        const bool available = selected_.worker_available();
                        if (!available) fixture_error("exit event preceded the queued adoption marker");
                        event("adoption-handler-drained", available ?
                              ",\"workerAvailable\":true" : ",\"workerAvailable\":false");
                    });
                    event("adoption-queued");
                    accept_healthy_successor();
                } else if (scenario_ == "before-adoption") {
                    held_.emplace(std::move(peer));
                    event("peer-held");
                } else {
                    refuse(std::move(peer));
                }
            });
        accept_thread_ = std::thread([this] { accept_io_.run(); });
        event("accept-armed", ",\"port\":" + std::to_string(port));
        if (pre_post_scenario()) {
            check(allocation_pause_.wait(), "real adopt_socket allocation pause timed out");
            check(allocation_pause_.bytes() == expected_allocation_bytes_,
                  "first adoption allocation is not the calibrated socket transport");
            check(selected_.worker_available(), "Worker exited before pre-post pause");
            event("adoption-pre-post-paused", ",\"workerAvailable\":true,\"allocationBytes\":" +
                  std::to_string(allocation_pause_.bytes()) + ",\"expectedAllocationBytes\":" +
                  std::to_string(expected_allocation_bytes_));
        }
        check(command() == "worker-exit", "expected worker-exit command");
        if (scenario_ == "pre-post-stop") selected_.request_stop();
        if (scenario_ == "pre-post-drain") selected_.begin_drain(0);
        if (scenario_ == "queued-before-exit") {
            check(selected_.worker_available(), "paused reactor already observed Worker exit");
            event("worker-exit-pending");
            check(command() == "release", "expected reactor release command");
            release_reactor();
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(6);
        while (selected_.worker_available() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(!selected_.worker_available(), "Worker exit was not observed");
        event("selected-unavailable");
        if (pre_post_scenario()) {
            selected_wait_ok_ = selected_.wait(&error);
            check(selected_wait_ok_, "selected wait before actual post failed");
            auto& context = static_cast<boost::asio::io_context&>(boost::asio::query(
                selected_.accept_executor(), boost::asio::execution::context));
            check(context.stopped(), "owner reactor must finish before producer resumes");
            event("selected-joined-before-post", ",\"workerAvailable\":false,\"reactorStopped\":true");
            check(command() == "release", "expected adoption allocation release");
            allocation_pause_.release();
        }
        if (scenario_ == "before-adoption") {
            check(command() == "adopt", "expected adopt command");
            boost::asio::post(accept_io_, [this] {
                if (!held_) {
                    fixture_error("adopt command has no accepted peer");
                    return;
                }
                auto peer = std::move(*held_);
                held_.reset();
                refuse(std::move(peer));
            });
        }
        check(command() == "stop", "expected stop command");
        stop();
        check(!failed_.load(), "asynchronous fixture assertion failed");
        check(selected_completions_ == 1, "selected accept must complete once");
        if (scenario_ == "cancel-pending") {
            check(cancelled_ == 1 && refused_ == 0 && healthy_accepted_ == 0,
                  "pending accept did not complete by cancellation alone");
        } else if (pre_post_scenario()) {
            check(cancelled_ == 0 && refused_ == 1 && queued_adoptions_ == 0 && healthy_accepted_ == 1,
                  "finished reactor must refuse an adoption paused before post");
        } else if (adopted_scenario()) {
            check(cancelled_ == 0 && refused_ == 0 && queued_adoptions_ == 1 &&
                      healthy_accepted_ == 1, "queued adoption and healthy successor must each occur once");
        } else {
            check(cancelled_ == 0 && refused_ == 1 && healthy_accepted_ == 1,
                  "refusal and healthy successor must each occur once");
        }
        check(selected_wait_ok_ && healthy_wait_ok_, "server wait failed");
        event("stopped", ",\"selectedCompletions\":" +
              std::to_string(selected_completions_) + ",\"cancelled\":" +
              std::to_string(cancelled_) + ",\"refused\":" +
              std::to_string(refused_) + ",\"queuedAdoptions\":" +
              std::to_string(queued_adoptions_) + ",\"healthyAccepted\":" +
              std::to_string(healthy_accepted_));
        return 0;
    }

private:
    bool pre_post_scenario() const { return scenario_.starts_with("pre-post-"); }
    bool adopted_scenario() const {
        return scenario_ == "queued-before-exit" || scenario_ == "idle-after-adoption" ||
               scenario_ == "keepalive-after-adoption";
    }
    static capsid::host::SingleWorkerServerOptions external_options(const char* worker) {
        auto options = make_options(worker, -1);
        options.external_accept = true;
        options.write_ready_record = false;
        options.install_process_signals = false;
        return options;
    }
    static void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }
    static std::string command() {
        std::string line;
        check(static_cast<bool>(std::getline(std::cin, line)), "driver input closed");
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    }
    void event(const std::string& stage, const std::string& fields = "") {
        std::lock_guard<std::mutex> lock(output_mutex_);
        std::cout << "{\"stage\":\"" << stage << "\"" << fields << "}" << std::endl;
    }
    void fixture_error(const char* message) {
        failed_.store(true);
        std::cerr << "external accept fixture: " << message << std::endl;
        event("fixture-error");
    }
    void refuse(boost::asio::ip::tcp::socket peer) {
        if (selected_.worker_available()) {
            fixture_error("selected Worker still available at refusal barrier");
            return;
        }
        if (selected_.adopt_socket(std::move(peer))) {
            fixture_error("unavailable Worker accepted a peer");
            return;
        }
        ++refused_;
        event("peer-refused");
        accept_healthy_successor();
    }
    void accept_healthy_successor() {
        // Reuse the same listener for a new connection to a different, live
        // shard. The rejected connection itself is never retried or rerouted.
        acceptor_.async_accept(healthy_.accept_executor(),
            [this](boost::system::error_code ec, boost::asio::ip::tcp::socket next) {
                if (accept_stopping_ && ec == boost::asio::error::operation_aborted) return;
                if (ec || next.get_executor() != healthy_.accept_executor() ||
                    !healthy_.adopt_socket(std::move(next))) {
                    fixture_error("healthy successor adoption failed");
                    return;
                }
                ++healthy_accepted_;
                event("healthy-adopted");
            });
    }
    void release_reactor() noexcept {
        {
            std::lock_guard<std::mutex> lock(reactor_mutex_);
            reactor_released_ = true;
        }
        reactor_condition_.notify_all();
    }
    void stop() noexcept {
        if (stopped_) return;
        stopped_ = true;
        release_reactor();
        allocation_pause_.release();
        if (accept_thread_.joinable()) {
            boost::asio::post(accept_io_, [this] {
                accept_stopping_ = true;
                boost::system::error_code ignored;
                acceptor_.close(ignored);
                held_.reset();
                work_.reset();
            });
            accept_thread_.join();
        } else {
            boost::system::error_code ignored;
            acceptor_.close(ignored);
            work_.reset();
        }
        selected_.request_stop();
        healthy_.request_stop();
        std::string error;
        selected_wait_ok_ = selected_.wait(&error);
        healthy_wait_ok_ = healthy_.wait(&error);
    }

    capsid::host::SingleWorkerServer selected_;
    capsid::host::SingleWorkerServer healthy_;
    boost::asio::io_context accept_io_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    std::string scenario_;
    std::optional<boost::asio::ip::tcp::socket> held_;
    std::thread accept_thread_;
    std::mutex output_mutex_;
    std::mutex reactor_mutex_;
    std::condition_variable reactor_condition_;
    bool reactor_paused_ = false;
    bool reactor_released_ = false;
    adoption_allocation_probe::Pause allocation_pause_;
    std::shared_ptr<boost::asio::ip::tcp::socket> allocation_calibration_;
    std::size_t expected_allocation_bytes_ = 0;
    bool accept_stopping_ = false;  // Accept-io thread only, including cancellation completion.
    unsigned selected_completions_ = 0;
    unsigned cancelled_ = 0;
    unsigned refused_ = 0;
    unsigned queued_adoptions_ = 0;
    unsigned healthy_accepted_ = 0;
    std::atomic<bool> failed_ = false;
    bool stopped_ = false;
    bool selected_wait_ok_ = false;
    bool healthy_wait_ok_ = false;
};

template <typename Server>
void exercise_lifecycle(Server* server,
                        const std::vector<std::uint8_t>& bundle,
                        int ready_read_fd,
                        int ready_write_fd) {
    if constexpr (requires(Server& value,
                           const std::vector<std::uint8_t>& bytes,
                           std::string* error) {
                      { value.start(bytes, error) } -> std::same_as<bool>;
                      value.request_stop();
                      { value.wait(error) } -> std::same_as<bool>;
                  }) {
        std::string error;
        require(server->start(bundle, &error),
                "controllable server start failed: " + error);
        require(!server->start(bundle, &error),
                "server accepted a duplicate start");
        close(ready_write_fd);
        const std::uint16_t port = ready_port(read_ready_line(ready_read_fd));
        require_http_response(port);

        const auto began = std::chrono::steady_clock::now();
        server->request_stop();
        server->request_stop();
        require(server->wait(&error), "server wait failed: " + error);
        require(std::chrono::steady_clock::now() - began <
                    std::chrono::seconds(2),
                "server stop/wait exceeded its bounded shutdown window");
    } else {
        (void)server;
        (void)bundle;
        (void)ready_read_fd;
        close(ready_write_fd);
        fail("SingleWorkerServer has no start/request_stop/wait lifecycle");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        const std::string scenario = argv[2];
        require(scenario == "before-completion" || scenario == "before-adoption" ||
                    scenario == "cancel-pending" || scenario == "queued-before-exit" ||
                    scenario == "idle-after-adoption" || scenario == "keepalive-after-adoption" ||
                    scenario == "pre-post-worker-exit" || scenario == "pre-post-stop" || scenario == "pre-post-drain",
                "unknown external accept scenario");
        try {
            ExternalAcceptExitFixture fixture(argv[1], scenario);
            return fixture.run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << error.what() << std::endl;
            return 1;
        }
    }
    require(argc == 2, "expected capsid-worker path");
    int ready[2];
    require(capsid::win32::create_socket_pair(ready), "cannot create READY pipe");

    const std::string source =
        "export default { fetch: () => new Response('lifecycle-ok') };";
    const std::vector<std::uint8_t> bundle(source.begin(), source.end());

    capsid::host::SingleWorkerServer server(make_options(argv[1], ready[1]));
    exercise_lifecycle(&server, bundle, ready[0], ready[1]);
    close(ready[0]);

    // The public facade, not Impl::~Impl(), owns the last reliable chance
    // to stop: long-lived accept/signal handlers retain shared_ptr<Impl> and
    // can otherwise prevent Impl's destructor from ever starting.
    int destructor_ready[2];
    require(capsid::win32::create_socket_pair(destructor_ready),
            "cannot create destructor READY pipe");
    std::uint16_t destructor_port = 0;
    const auto destructor_began = std::chrono::steady_clock::now();
    {
        capsid::host::SingleWorkerServer abandoned(
            make_options(argv[1], destructor_ready[1]));
        std::string error;
        require(abandoned.start(bundle, &error),
                "cannot start destructor lifecycle fixture: " + error);
        close(destructor_ready[1]);
        destructor_port = ready_port(read_ready_line(destructor_ready[0]));
        require_http_response(destructor_port);
        // No explicit request_stop/wait: ~SingleWorkerServer must perform
        // the same bounded teardown and drain handlers retaining Impl.
    }
    require(std::chrono::steady_clock::now() - destructor_began <
                std::chrono::seconds(2),
            "running server destructor exceeded its bounded shutdown window");
    require_port_closed(destructor_port);
    close(destructor_ready[0]);

    // A failure after bind (READY publication here) must unwind the listener
    // and worker before start() returns. StaticPoolServer depends on this to
    // make an N-shard startup failure atomic while shard objects remain alive.
    const std::uint16_t failed_start_port = reserve_test_port();
#if defined(_WIN32)
    const int read_only_ready_fd = _open("NUL", _O_RDONLY);
#else
    const int read_only_ready_fd =
        open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
    require(read_only_ready_fd >= 0,
            "cannot create failed-READY lifecycle fixture");
    auto failed_options = make_options(argv[1], read_only_ready_fd);
    failed_options.listen_port = failed_start_port;
    capsid::host::SingleWorkerServer failed_start(
        std::move(failed_options));
    std::string failed_error;
    const auto failed_began = std::chrono::steady_clock::now();
    require(!failed_start.start(bundle, &failed_error),
            "server accepted an unwritable READY descriptor");
    require(std::chrono::steady_clock::now() - failed_began <
                std::chrono::seconds(2),
            "failed start exceeded its bounded cleanup window");
    require_port_bindable(failed_start_port);
    close(read_only_ready_fd);

    // Moving a socket preserves its executor. A socket accepted on another
    // reactor must never enter this shard: its callbacks would race the
    // shard's Worker events and mutate the same HTTP/WebSocket session.
    {
        auto external_options = make_options(argv[1], -1);
        external_options.external_accept = true;
        external_options.write_ready_record = false;
        external_options.install_process_signals = false;
        capsid::host::SingleWorkerServer external(std::move(external_options));
        std::string error;
        require(external.start(bundle, &error),
                "cannot start external-accept fixture: " + error);
        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;
        asio::io_context foreign_io;
        tcp::acceptor acceptor(foreign_io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        tcp::socket client(foreign_io);
        client.connect(acceptor.local_endpoint());
        tcp::socket peer = acceptor.accept();
        require(peer.is_open(), "foreign-executor fixture has no connection");
        const bool adopted = external.adopt_socket(std::move(peer));

        // Drain even the old implementation's wrongly adopted operation
        // before reporting RED, so the regression itself reaps its worker.
        auto guard = asio::make_work_guard(foreign_io);
        std::thread foreign_thread([&] { foreign_io.run(); });
        external.request_stop();
        const bool stopped = external.wait(&error);
        guard.reset();
        foreign_io.stop();
        foreign_thread.join();
        require(stopped, "external-accept fixture did not stop: " + error);
        require(!adopted,
                "shard accepted a socket whose callbacks run on another reactor");
    }

    // Positive ownership: a peer created directly on the shard's reactor
    // — the same async_accept(executor, ...) path the Windows static pool
    // uses — is adopted and served on that reactor, together with the
    // Worker events for the same session.
    {
        auto owned_options = make_options(argv[1], -1);
        owned_options.external_accept = true;
        owned_options.write_ready_record = false;
        owned_options.install_process_signals = false;
        capsid::host::SingleWorkerServer owned(std::move(owned_options));
        std::string error;
        require(owned.start(bundle, &error),
                "cannot start owned-accept fixture: " + error);
        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;
        asio::io_context foreign_io;
        tcp::acceptor acceptor(foreign_io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        tcp::socket client(foreign_io);
        client.connect(acceptor.local_endpoint());
        std::optional<tcp::socket> peer;
        bool accept_failed = false;
        acceptor.async_accept(
            owned.accept_executor(),
            [&peer, &accept_failed](boost::system::error_code ec,
                                    tcp::socket accepted) {
                if (ec) {
                    accept_failed = true;
                } else {
                    peer.emplace(std::move(accepted));
                }
            });
        foreign_io.run();
        require(!accept_failed, "owned-executor accept failed");
        require(peer.has_value() && peer->is_open(),
                "owned-executor fixture has no connection");
        require(owned.adopt_socket(std::move(*peer)),
                "shard refused a socket born on its own reactor");

        // The adopted connection must serve real traffic: its network
        // callbacks and the worker's response share the shard reactor.
#if defined(_WIN32)
        const DWORD io_timeout = 5000;
        const char* timeout_data =
            reinterpret_cast<const char*>(&io_timeout);
        const int timeout_size = static_cast<int>(sizeof(io_timeout));
#else
        const timeval io_timeout = {5, 0};
        const auto* timeout_data = &io_timeout;
        const socklen_t timeout_size = sizeof(io_timeout);
#endif
        for (const int option : {SO_RCVTIMEO, SO_SNDTIMEO}) {
            if (::setsockopt(client.native_handle(), SOL_SOCKET, option,
                             timeout_data, timeout_size) != 0) {
                fail("cannot set owned-socket HTTP timeout");
            }
        }
        const std::string request =
            "GET /@capsid/orders/lifecycle HTTP/1.1\r\n"
            "Host: public.example\r\n"
            "Connection: close\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::string response;
        char bytes[2048];
        boost::system::error_code read_error;
        for (;;) {
            const std::size_t count =
                client.read_some(asio::buffer(bytes), read_error);
            response.append(bytes, count);
            if (read_error == asio::error::eof) {
                break;
            }
            require(!read_error, "cannot read owned-socket HTTP response: " +
                                     read_error.message());
        }
        client.close();
        owned.request_stop();
        require(owned.wait(&error),
                "owned-accept fixture did not stop: " + error);
        require(response.find(" 200 ") != std::string::npos &&
                    response.find("lifecycle-ok") != std::string::npos,
                "owned-reactor connection did not serve HTTP");
    }

    // Stop has reached the reactor before the accept owner checks adoption.
    // worker_available() is a relaxed atomic observation, not an acquire of
    // reactor-local state; the owner must not read shutting_down_ directly.
    {
        auto options = make_options(argv[1], -1);
        options.external_accept = true;
        options.write_ready_record = false;
        options.install_process_signals = false;
        capsid::host::SingleWorkerServer owner(std::move(options));
        std::string error;
        require(owner.start(bundle, &error),
                "cannot start stop/adopt fixture: " + error);
        namespace asio = boost::asio;
        using tcp = asio::ip::tcp;
        asio::io_context accept_io;
        tcp::acceptor acceptor(accept_io,
            tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        std::optional<tcp::socket> peer;
        acceptor.async_accept(owner.accept_executor(),
            [&peer](boost::system::error_code ec, tcp::socket socket) {
                require(!ec, "stop/adopt accept failed");
                peer.emplace(std::move(socket));
            });
        tcp::socket client(accept_io);
        client.connect(acceptor.local_endpoint());
        accept_io.run();
        require(peer.has_value() && peer->is_open(),
                "stop/adopt actual connection absent");
        owner.request_stop();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (owner.worker_available() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        require(!owner.worker_available(),
                "stop did not reach the owning reactor");
        const bool accepted = owner.adopt_socket(std::move(*peer));
        require(owner.wait(&error),
                "stop/adopt owner did not join: " + error);
        require(!accepted, "stopping shard accepted a new transport");

        // Refusal must also close the real peer. A return value alone does
        // not prove the accepted transport was released.
        client.non_blocking(true);
        const auto close_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool closed = false;
        while (std::chrono::steady_clock::now() < close_deadline) {
            char byte = 0;
            boost::system::error_code ec;
            const auto count = client.read_some(asio::buffer(&byte, 1), ec);
            if (ec == asio::error::eof || ec == asio::error::connection_reset) {
                closed = true;
                break;
            }
            require(count == 0 && (ec == asio::error::would_block ||
                                   ec == asio::error::try_again),
                    "refused transport received unexpected data or error");
            std::this_thread::yield();
        }
        client.close();
        require(closed, "refused accepted transport did not close");
    }

    std::cout << "PASS" << std::endl;
    return 0;
}
