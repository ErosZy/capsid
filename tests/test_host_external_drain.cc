// Exercise the real external-admission API while its owner is paused by a
// test-owned Asio handler. Production code has no scheduling hook.
#include "host/single_worker_server.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace admission_probe {
class Pause {
public:
  explicit Pause(bool record_only = false) : record_only_(record_only) {}
  void enter(std::size_t bytes) {
    std::unique_lock<std::mutex> lock(mutex_);
    bytes_ = bytes;
    entered_ = true;
    condition_.notify_all();
    if (!record_only_)
      condition_.wait(lock, [this] { return released_; });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(6),
                               [this] { return entered_; });
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
// This TLS probe is armed immediately around the actual try_adopt_socket
// invocation, just as in the existing external-accept lifecycle fixture.
thread_local Pause *next = nullptr;
} // namespace admission_probe

void *operator new(std::size_t bytes) {
  if (auto *pause = std::exchange(admission_probe::next, nullptr))
    pause->enter(bytes);
  if (void *memory = std::malloc(bytes ? bytes : 1))
    return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
  return std::malloc(bytes ? bytes : 1);
}
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept {
  return std::malloc(bytes ? bytes : 1);
}
#if defined(__GNUC__)
#define CAPSID_TEST_ALLOCATION_BOUNDARY __attribute__((noinline))
#else
#define CAPSID_TEST_ALLOCATION_BOUNDARY
#endif
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void *memory) noexcept {
  std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void *memory) noexcept {
  std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete(void *memory,
                                                     std::size_t) noexcept {
  std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void operator delete[](void *memory,
                                                       std::size_t) noexcept {
  std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void
operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
CAPSID_TEST_ALLOCATION_BOUNDARY void
operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
#undef CAPSID_TEST_ALLOCATION_BOUNDARY

namespace {
using tcp = boost::asio::ip::tcp;
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string boolean(bool value) { return value ? "true" : "false"; }
struct DriverClosed {};
std::string command() {
  std::string line;
  if (!std::getline(std::cin, line))
    throw DriverClosed{};
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}
capsid::host::SingleWorkerServerOptions options(const char *worker) {
  capsid::host::SingleWorkerServerOptions result;
  result.worker_path = worker;
  result.source_bundle_path = "external-drain-inline";
  result.source_name = "file://orders/v1/bundle.mjs";
  result.application = "orders";
  result.listen_address = "127.0.0.1";
  result.listen_port = 0;
  result.public_scheme = "http";
  result.public_authority = "public.example";
  result.request_timeout_ms = 6000;
  result.initial_stream_window = 64U * 1024U;
  result.strict_sandbox = false;
  result.external_accept = true;
  result.write_ready_record = false;
  result.install_process_signals = false;
  return result;
}

class Fixture {
public:
  Fixture(const char *worker, std::string scenario)
      : selected_(options(worker)), healthy_(options(worker)),
        selected_acceptor_(accept_io_), healthy_acceptor_(accept_io_),
        work_(accept_io_.get_executor()), scenario_(std::move(scenario)) {}
  ~Fixture() { stop(false); }

  int run() {
    const std::string source = R"js(
const instance = crypto.randomUUID();
export default {fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/ws')) {
        const pair = new WebSocketPair(); pair[1].accept(); pair[1].binaryType = 'arraybuffer';
        pair[1].onmessage = event => pair[1].send(event.data);
        return new Response(null, {status: 101, webSocket: pair[0]});
    }
    return new Response(JSON.stringify({challenge: url.searchParams.get('challenge'), instance}));
}};
)js";
    const std::vector<std::uint8_t> bundle(source.begin(), source.end());
    std::string error;
    require(selected_.start(bundle, &error), "start selected: " + error);
    event("selected-ready");
    bool completed = false;
    try {
      require(command() == "arm", "expected listener setup");
      require(healthy_.start(bundle, &error), "start healthy: " + error);
      admission_probe::Pause calibration(true);
      admission_probe::next = &calibration;
      calibration_ = std::make_shared<tcp::socket>(selected_.accept_executor());
      admission_probe::next = nullptr;
      expected_allocation_ = calibration.bytes();
      require(expected_allocation_ > 0, "calibrate actual socket allocation");
      bind(selected_acceptor_);
      bind(healthy_acceptor_);
      accept_selected();
      accept_healthy();
      accept_thread_ = std::thread([this] { accept_io_.run(); });
      event("listeners-ready",
            ",\"port\":" +
                std::to_string(selected_acceptor_.local_endpoint().port()) +
                ",\"healthyPort\":" +
                std::to_string(healthy_acceptor_.local_endpoint().port()));
      require(command() == "warm", "expected established WebSocket barrier");
      const bool blocked = scenario_ != "owner-drain-before-submit";
      if (blocked) {
        boost::asio::post(selected_.accept_executor(),
                          [this] { owner_pause_.enter(0); });
        require(owner_pause_.wait(), "pause actual owner reactor");
      }
      event("owner-positioned", ",\"blocked\":" + boolean(blocked) +
                                    ",\"workerAvailable\":" +
                                    boolean(selected_.worker_available()));
      require(command() == "prepare", "expected pending admission setup");
      if (scenario_ == "drain-before-submit" ||
          scenario_ == "owner-drain-before-submit")
        begin_drain();
      if (!blocked)
        boost::asio::post(selected_.accept_executor(), [this] {
          event("owner-drain-processed",
                ",\"workerAvailable\":" +
                    boolean(selected_.worker_available()) + ",\"draining\":" +
                    boolean(selected_.drain_metrics().draining));
        });
      pending_armed_.store(true, std::memory_order_release);
      event("pending-armed");
      if (scenario_ == "drain-during-prepare") {
        require(allocation_pause_.wait(),
                "actual try_adopt_socket allocation pause");
        require(allocation_pause_.bytes() == expected_allocation_,
                "actual transport allocation size");
        require(selected_.worker_available() &&
                    !selected_.drain_metrics().draining,
                "live preflight precedes drain request");
        event("adoption-prepared",
              ",\"workerAvailable\":true,\"draining\":false,"
              "\"allocationBytes\":" +
                  std::to_string(allocation_pause_.bytes()) +
                  ",\"expectedAllocationBytes\":" +
                  std::to_string(expected_allocation_));
      }
      if (scenario_ == "drain-during-prepare" ||
          scenario_ == "queued-before-drain") {
        require(command() == "drain", "expected concurrent drain request");
        begin_drain();
      }
      if (scenario_ == "drain-during-prepare") {
        require(command() == "release-allocation", "expected producer release");
        allocation_pause_.release();
      }
      require(command() == "release-owner", "expected owner release");
      owner_pause_.release();
      boost::asio::post(selected_.accept_executor(), [this] {
        event(
            "owner-released",
            ",\"workerAvailable\":" + boolean(selected_.worker_available()) +
                ",\"draining\":" + boolean(selected_.drain_metrics().draining));
      });
      require(command() == "finish", "expected completed client assertions");
      completed = true;
    } catch (const DriverClosed &) {
      // Preserve the driver's primary network assertion and still join
      // every owner and child normally. EOF is never a successful case.
    }
    stop(completed && scenario_ != "healthy-control");
    require(selected_wait_ok_ && healthy_wait_ok_, "both servers joined");
    const auto metrics = selected_.drain_metrics();
    if (completed) {
      require(!failed_.load() && warm_accepted_ == 1 && pending_calls_ == 1 &&
                  healthy_accepted_ == 2,
              "actual warm, pending and healthy admission counts");
      if (scenario_ != "healthy-control")
        require(metrics.draining && metrics.finished &&
                    metrics.forced_cancellations == 0,
                "selected Worker drains naturally after WebSocket close");
    }
    event("stopped",
          ",\"selectedAvailable\":" + boolean(selected_.worker_available()) +
              ",\"healthyAvailable\":" + boolean(healthy_.worker_available()) +
              ",\"warmAccepted\":" + std::to_string(warm_accepted_) +
              ",\"pendingCalls\":" + std::to_string(pending_calls_) +
              ",\"healthyAccepted\":" + std::to_string(healthy_accepted_) +
              ",\"draining\":" + boolean(metrics.draining) + ",\"finished\":" +
              boolean(metrics.finished) + ",\"forcedCancellations\":" +
              std::to_string(metrics.forced_cancellations));
    return 0;
  }

private:
  static void bind(tcp::acceptor &acceptor) {
    acceptor.open(tcp::v4());
    acceptor.bind(tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    acceptor.listen();
  }
  void event(const char *stage, const std::string &extra = {}) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    std::cout << "{\"stage\":\"" << stage << '"' << extra << "}" << std::endl;
  }
  void error() {
    failed_.store(true);
    event("fixture-error");
  }
  void begin_drain() {
    selected_.begin_drain(15000);
    const auto metrics = selected_.drain_metrics();
    require(
        metrics.draining && !metrics.finished && selected_.worker_available(),
        "drain call returned while existing WebSocket keeps Worker available");
    event("drain-requested",
          ",\"draining\":true,\"finished\":false,\"workerAvailable\":true");
  }
  void accept_selected() {
    selected_acceptor_.async_accept(
        selected_.accept_executor(),
        [this](boost::system::error_code ec, tcp::socket peer) {
          if (stopping_ && ec == boost::asio::error::operation_aborted)
            return;
          if (ec || peer.get_executor() != selected_.accept_executor()) {
            error();
            return;
          }
          if (!pending_armed_.load(std::memory_order_acquire)) {
            if (!selected_.adopt_socket(std::move(peer))) {
              error();
              return;
            }
            ++warm_accepted_;
            accept_selected();
            return;
          }
          const auto original_handle = peer.native_handle();
          bool accepted = false;
          try {
            if (scenario_ == "drain-during-prepare")
              admission_probe::next = &allocation_pause_;
            accepted = selected_.try_adopt_socket(peer);
            admission_probe::next = nullptr;
          } catch (...) {
            admission_probe::next = nullptr;
            error();
            return;
          }
          ++pending_calls_;
          const bool retained =
              peer.is_open() && peer.native_handle() == original_handle &&
              peer.get_executor() == selected_.accept_executor();
          const bool open = peer.is_open();
          boost::system::error_code ignored;
          peer.close(ignored);
          event("adoption-returned",
                ",\"accepted\":" + boolean(accepted) +
                    ",\"retained\":" + boolean(retained) +
                    ",\"openAfter\":" + boolean(open) + ",\"draining\":" +
                    boolean(selected_.drain_metrics().draining) +
                    ",\"workerAvailable\":" +
                    boolean(selected_.worker_available()));
        });
  }
  void accept_healthy() {
    healthy_acceptor_.async_accept(
        healthy_.accept_executor(),
        [this](boost::system::error_code ec, tcp::socket peer) {
          if (stopping_ && ec == boost::asio::error::operation_aborted)
            return;
          if (ec || !healthy_.adopt_socket(std::move(peer))) {
            error();
            return;
          }
          ++healthy_accepted_;
          accept_healthy();
        });
  }
  void stop(bool natural) noexcept {
    if (stopped_)
      return;
    stopped_ = true;
    admission_probe::next = nullptr;
    owner_pause_.release();
    allocation_pause_.release();
    if (accept_thread_.joinable()) {
      boost::asio::post(accept_io_, [this] {
        stopping_ = true;
        boost::system::error_code ignored;
        selected_acceptor_.close(ignored);
        healthy_acceptor_.close(ignored);
        work_.reset();
      });
      accept_thread_.join();
    } else {
      boost::system::error_code ignored;
      selected_acceptor_.close(ignored);
      healthy_acceptor_.close(ignored);
      work_.reset();
    }
    if (!natural)
      selected_.request_stop();
    healthy_.request_stop();
    std::string message;
    selected_wait_ok_ = selected_.wait(&message);
    healthy_wait_ok_ = healthy_.wait(&message);
  }

  capsid::host::SingleWorkerServer selected_;
  capsid::host::SingleWorkerServer healthy_;
  boost::asio::io_context accept_io_;
  tcp::acceptor selected_acceptor_;
  tcp::acceptor healthy_acceptor_;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_;
  const std::string scenario_;
  std::thread accept_thread_;
  std::mutex output_mutex_;
  admission_probe::Pause owner_pause_;
  admission_probe::Pause allocation_pause_;
  std::shared_ptr<tcp::socket> calibration_;
  std::size_t expected_allocation_ = 0;
  std::atomic<bool> pending_armed_{false};
  std::atomic<bool> failed_{false};
  unsigned warm_accepted_ = 0;
  unsigned pending_calls_ = 0;
  unsigned healthy_accepted_ = 0;
  bool stopping_ =
      false; // Accept owner only, including cancellation callbacks.
  bool stopped_ = false;
  bool selected_wait_ok_ = false;
  bool healthy_wait_ok_ = false;
};
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 3, "expected actual Worker and scenario");
    const std::string scenario = argv[2];
    require(scenario == "drain-before-submit" ||
                scenario == "drain-during-prepare" ||
                scenario == "queued-before-drain" ||
                scenario == "owner-drain-before-submit" ||
                scenario == "healthy-control",
            "known drain scenario");
    return Fixture(argv[1], scenario).run();
  } catch (const std::exception &error) {
    std::cerr << "external drain fixture: " << error.what() << std::endl;
    return 1;
  }
}
