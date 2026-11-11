// Real TCP backpressure through the public API. Peeking never consumes bytes;
// admission must recover before the stalled client starts reading its body.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#include "host/single_worker_server.h"
#include "host/static_pool_server.h"
#include "win32_compat.h"
#include "worker_exit_assertions.h"
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
constexpr std::size_t body_size = 8U * 1024U * 1024U;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
long elapsed(Clock::time_point start) {
  return static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count());
}
std::string quote(const std::string &value) {
  std::string result = "\"";
  for (const char ch : value) {
    if (ch == '"' || ch == '\\')
      result += '\\';
    result += ch >= 32 ? ch : '?';
  }
  return result + '"';
}
struct Report {
  std::string scenario;
  std::size_t workers = 1, busy = 0, recovered = 0;
  long action_ms = 0;
  bool cleanup = false, unread_until_action = false;
  std::vector<std::size_t> wire_bytes, body_bytes;
  std::vector<int> receive_buffers;
  std::vector<capsid::WorkerExitStatus> worker_exits;
  capsid::host::SingleWorkerServer::DrainMetrics drain;
  void print(bool complete, const std::string &error) const {
    std::cout << "{\"complete\":" << (complete ? "true" : "false")
              << ",\"scenario\":" << quote(scenario)
              << ",\"workers\":" << workers << ",\"busyResponses\":" << busy
              << ",\"recoveredInstances\":" << recovered
              << ",\"actionMs\":" << action_ms
              << ",\"normalCleanup\":" << (cleanup ? "true" : "false")
              << ",\"unreadUntilAction\":"
              << (unread_until_action ? "true" : "false")
              << ",\"declaredBodyBytes\":" << body_size;
    const auto array = [](const auto &values) {
      std::cout << '[';
      for (std::size_t i = 0; i < values.size(); ++i)
        std::cout << (i ? "," : "") << values[i];
      std::cout << ']';
    };
    std::cout << ",\"wireBytes\":";
    array(wire_bytes);
    std::cout << ",\"bodyBytes\":";
    array(body_bytes);
    std::cout << ",\"receiveBuffers\":";
    array(receive_buffers);
    std::cout << ",\"workerExits\":[";
    for (std::size_t i = 0; i < worker_exits.size(); ++i) {
      if (i) std::cout << ',';
      capsid_test::print_worker_exit(worker_exits[i]);
    }
    std::cout << ']';
    std::cout << ",\"drain\":{\"draining\":"
              << (drain.draining ? "true" : "false")
              << ",\"finished\":" << (drain.finished ? "true" : "false")
              << ",\"forcedCancellations\":" << drain.forced_cancellations
              << "},\"error\":" << quote(error) << "}" << std::endl;
  }
};
class Server {
public:
  Server(const char *worker, bool pool, std::uint64_t write_ms,
         std::uint64_t request_ms, Report &report)
      : report_(report) {
    require(capsid::win32::create_socket_pair(ready_), "private READY pair");
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker;
    options.source_bundle_path = "write-timeout-inline";
    options.source_name = "file://write-timeout/v1/bundle.mjs";
    options.application = "orders";
    options.listen_address = "127.0.0.1";
    options.public_scheme = "http";
    options.public_authority = "public.example";
    options.request_timeout_ms = request_ms;
    options.write_timeout_ms = write_ms;
    options.initial_stream_window = 64U * 1024U;
    options.max_inflight_per_worker = 1;
    options.max_streaming_inflight_per_worker = 1;
    options.install_process_signals = false;
    options.ready_fd = ready_[1];
    const std::string source = std::string("const cooperative = ") +
        (report.scenario == "worker-deadline-cooperative" ? "true;\n" : "false;\n") + R"js(
const instance = crypto.randomUUID();
export default {async fetch(request) {
    const url = new URL(request.url);
    const headers = {'x-worker-instance': instance};
    if (url.pathname.endsWith('/big')) {
        const body = new Uint8Array(8 * 1024 * 1024); body.fill(97);
        headers['content-length'] = String(body.byteLength);
        return new Response(body, {headers});
    }
    if (url.pathname.endsWith('/slow')) await new Promise(resolve => {
        const timer = setTimeout(resolve, 600000);
        if (cooperative) request.signal.addEventListener('abort', () => {
            clearTimeout(timer); resolve();
        }, {once: true});
    });
    return new Response('healthy:' + url.searchParams.get('challenge'), {headers});
}};
)js";
    bundle_.assign(source.begin(), source.end());
    if (pool) {
      capsid::host::StaticPoolServerOptions all;
      all.worker_options = std::move(options);
      all.workers = 2;
      pool_ = std::make_unique<capsid::host::StaticPoolServer>(std::move(all));
    } else
      single_ = std::make_unique<capsid::host::SingleWorkerServer>(
          std::move(options));
  }
  ~Server() {
    if (!joined_) {
      stop();
      std::string error;
      report_.cleanup = pool_ ? pool_->wait(&error) : single_->wait(&error);
    }
    close(ready_[0]);
    close(ready_[1]);
  }
  void start() {
    std::string error;
    require(pool_ ? pool_->start(bundle_, &error)
                  : single_->start(bundle_, &error),
            "start actual server");
    std::string line;
    const auto start = Clock::now();
    while (line.find('\n') == std::string::npos) {
      require(elapsed(start) < 6000, "actual READY deadline");
      capsid_pollfd descriptor{};
      descriptor.fd = ready_[0];
      descriptor.events = POLLIN;
      const int polled = capsid::win32::capsid_poll(&descriptor, 1, 50);
      require(polled >= 0, "poll READY");
      if (!polled)
        continue;
      char data[1024];
      const auto count = capsid::win32::read_fd(ready_[0], data, sizeof(data));
      require(count > 0, "read READY");
      line.append(data, static_cast<std::size_t>(count));
      require(line.size() <= 1024, "bounded READY record");
    }
    const auto begin = line.find("\"port\":");
    require(begin != std::string::npos, "READY port");
    const auto value = std::stoul(line.substr(begin + 7));
    require(value > 0 && value <= 65535, "actual bound port");
    port = static_cast<std::uint16_t>(value);
    if (pool_)
      require(pool_->active_workers() == 2, "two actual Workers READY");
  }
  void stop() {
    if (pool_)
      pool_->request_stop();
    else
      single_->request_stop();
  }
  void begin_drain() { pool_->begin_drain(5000); }
  void join() {
    std::string error;
    report_.cleanup = pool_ ? pool_->wait(&error) : single_->wait(&error);
    joined_ = true;
    require(report_.cleanup, "normal server wait");
    if (pool_)
      require(pool_->active_workers() == 0, "all pool Workers stopped");
    report_.drain = pool_ ? pool_->drain_metrics() : single_->drain_metrics();
    report_.worker_exits = pool_ ? pool_->worker_exit_statuses()
                                : std::vector{single_->worker_exit_status()};
    require(report_.worker_exits.size() == report_.workers,
            "one actual OS exit per Worker");
    for (const auto &status : report_.worker_exits) {
      require(report_.scenario == "worker-deadline-independent"
                  ? capsid_test::natural_exit(status, 1) : status.clean(),
              "actual Worker exit disagrees with cancellation/drain contract");
    }
  }
  std::uint16_t port = 0;

private:
  Report &report_;
  int ready_[2] = {-1, -1};
  std::vector<std::uint8_t> bundle_;
  std::unique_ptr<capsid::host::SingleWorkerServer> single_;
  std::unique_ptr<capsid::host::StaticPoolServer> pool_;
  bool joined_ = false;
};
bool retry(boost::system::error_code ec) {
  return ec == asio::error::would_block || ec == asio::error::try_again;
}
tcp::socket connect(asio::io_context &io, std::uint16_t port, bool small) {
  tcp::socket socket(io);
  socket.open(tcp::v4());
  if (small)
    socket.set_option(asio::socket_base::receive_buffer_size(1024));
  socket.connect({asio::ip::address_v4::loopback(), port});
  return socket;
}
void send(tcp::socket &socket, const std::string &path) {
  const std::string bytes =
      "GET /@capsid/orders/" + path +
      " HTTP/1.1\r\nHost: public.example\r\nConnection: close\r\n\r\n";
  asio::write(socket, asio::buffer(bytes));
  socket.non_blocking(true);
}
std::string receive(tcp::socket &socket) {
  // This opens the receive window only after the no-read assertion.
  socket.set_option(asio::socket_base::receive_buffer_size(256 * 1024));
  std::string wire;
  const auto start = Clock::now();
  for (;;) {
    boost::system::error_code ec;
    char data[65536];
    const auto count = socket.read_some(asio::buffer(data), ec);
    wire.append(data, count);
    require(wire.size() <= body_size + 8192, "bounded actual response");
    if (ec == asio::error::eof || ec == asio::error::connection_reset)
      return wire;
    require(!ec || retry(ec), "client TCP read");
    require(elapsed(start) < 4000, "client response deadline");
    if (!count)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
http::response<http::string_body> parse(const std::string &wire,
                                        bool complete) {
  http::response_parser<http::string_body> parser;
  parser.body_limit(body_size);
  parser.eager(true);
  boost::system::error_code ec;
  const auto consumed = parser.put(asio::buffer(wire), ec);
  require(!ec && consumed == wire.size() && parser.is_header_done(),
          "actual HTTP response framing");
  require(parser.is_done() == complete,
          "expected complete or truncated actual response");
  if (!complete) {
    parser.put_eof(ec);
    require(ec == http::error::partial_message, "truncated HTTP body at EOF");
  }
  return parser.release();
}
std::string instance(const http::response<http::string_body> &response) {
  const std::string value(response["x-worker-instance"]);
  require(value.size() == 36, "actual Worker instance identity");
  return value;
}
http::response<http::string_body> health(asio::io_context &io, Server &server,
                                         std::size_t challenge) {
  auto socket = connect(io, server.port, false);
  send(socket, "check?challenge=" + std::to_string(challenge));
  auto response = parse(receive(socket), true);
  if (response.result_int() == 200)
    require(response.body() == "healthy:" + std::to_string(challenge),
            "dynamic healthy Worker response");
  return response;
}
void exercise(const char *worker, Report &report) {
  const auto &scenario = report.scenario;
  const bool pool = scenario.starts_with("pool-");
  const bool stop = scenario == "pool-stop-pending-write";
  const bool drain = scenario == "pool-drain-write-deadline";
  const bool disabled = scenario == "write-timeout-disabled";
  const bool independent = scenario == "worker-deadline-independent" ||
                           scenario == "worker-deadline-cooperative";
  const bool fast = scenario == "fast-response-untouched";
  report.workers = pool ? 2 : 1;
  Server server(worker, pool,
                disabled              ? 0
                : stop || independent ? 5000
                                      : 500,
                independent ? 200 : 10000, report);
  server.start();
  asio::io_context io;
  std::size_t challenge = 1;
  std::set<std::string> warm;
  for (std::size_t i = 0; i < report.workers; ++i) {
    const auto response = health(io, server, challenge++);
    require(response.result_int() == 200, "warm actual Worker");
    warm.insert(instance(response));
  }
  require(warm.size() == report.workers, "distinct actual Worker instances");
  if (independent || fast) {
    auto socket = connect(io, server.port, false);
    send(socket, independent ? "slow?challenge=99" : "check?challenge=99");
    const auto response = parse(receive(socket), true);
    require(response.result_int() == (independent ? 504U : 200U),
            "independent worker deadline or fast response status");
    if (fast)
      require(response.body() == "healthy:99", "fast response body");
  } else {
    std::vector<tcp::socket> stalled;
    std::set<std::string> writing;
    for (std::size_t i = 0; i < report.workers; ++i) {
      auto socket = connect(io, server.port, true);
      asio::socket_base::receive_buffer_size size;
      socket.get_option(size);
      require(size.value() > 0 && size.value() <= 65536,
              "bounded client receive buffer");
      report.receive_buffers.push_back(size.value());
      send(socket, "big");
      const auto start = Clock::now();
      for (;;) {
        char data[8192];
        boost::system::error_code ec;
        const auto count = socket.receive(asio::buffer(data),
                                          asio::socket_base::message_peek, ec);
        require(!ec || retry(ec), "peek actual pending response");
        const std::string peek(data, count);
        const auto end = peek.find("\r\n\r\n");
        if (end != std::string::npos && peek.size() > end + 4) {
          const auto response = parse(peek, false);
          require(response.result_int() == 200 &&
                      response[http::field::content_length] == "8388608",
                  "large response head and initial body before pause");
          writing.insert(instance(response));
          break;
        }
        require(elapsed(start) < 3000, "actual response start deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      stalled.push_back(std::move(socket));
    }
    require(writing == warm, "every actual Worker has a started response");
    for (std::size_t i = 0; i < report.workers; ++i) {
      require(health(io, server, challenge++).result_int() == 429,
              "started unread responses occupy all admission slots");
      ++report.busy;
    }
    const auto action = Clock::now();
    if (stop || drain) {
      for (int i = 0; i < 3; ++i) {
        if (stop)
          server.stop();
        else
          server.begin_drain();
      }
      server.join();
      report.action_ms = elapsed(action);
      require(report.action_ms < 2500, "pending write shutdown deadline");
      require(report.drain.forced_cancellations == 0,
              "write deadline completes drain without forced cancellation");
      if (drain)
        require(report.drain.draining && report.drain.finished,
                "active pool drain completes");
    } else if (disabled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
      require(health(io, server, challenge++).result_int() == 429,
              "disabled write deadline preserves unread pending request");
      report.action_ms = elapsed(action);
    } else {
      std::set<std::string> recovered;
      while (recovered != warm && elapsed(action) < 2500) {
        const auto response = health(io, server, challenge++);
        require(response.result_int() == 429 || response.result_int() == 200,
                "healthy or occupied admission response");
        if (response.result_int() == 200)
          recovered.insert(instance(response));
        if (recovered != warm)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      report.recovered = recovered.size();
      report.action_ms = elapsed(action);
      require(recovered == warm,
              "write deadline must release admission before client reads");
    }
    report.unread_until_action = true;
    for (auto &socket : stalled) {
      const auto wire = receive(socket);
      const auto response = parse(wire, disabled);
      require(response.result_int() == 200 &&
                  response[http::field::content_length] == "8388608",
              "original 200 response remains on wire");
      require(!response.body().empty() &&
                  response.body().find_first_not_of('a') == std::string::npos,
              "actual original body prefix");
      require(disabled ? response.body().size() == body_size
                       : response.body().size() < body_size,
              "completed disabled control or truncated cancelled body");
      report.wire_bytes.push_back(wire.size());
      report.body_bytes.push_back(response.body().size());
    }
    if (!stop && !drain) {
      const auto response = health(io, server, challenge++);
      require(response.result_int() == 200 && warm.contains(instance(response)),
              "healthy after actual socket cleanup");
    }
  }
  if (report.cleanup)
    return;
  server.stop();
  server.join();
}
} // namespace
int main(int argc, char **argv) {
  Report report;
  try {
    require(argc == 3, "expected scenario and actual Worker path");
    report.scenario = argv[1];
    const std::set<std::string> known{
        "write-timeout-cancels",      "worker-deadline-independent",
        "worker-deadline-cooperative",
        "fast-response-untouched",    "write-timeout-disabled",
        "pool-write-timeout-cancels", "pool-drain-write-deadline",
        "pool-stop-pending-write"};
    require(known.contains(report.scenario), "known write deadline scenario");
    exercise(argv[2], report);
    report.print(true, "");
    return 0;
  } catch (const std::exception &error) {
    report.print(false, error.what());
    return 1;
  }
}
