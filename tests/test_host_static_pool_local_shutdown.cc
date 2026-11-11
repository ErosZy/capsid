// Real pool close/drain with pending local HTTP heads. The separate private
// Session test controls write completion; this fixture has no scheduling hook.
#include "host/static_pool_server.h"
#include "win32_compat.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct DriverClosed {};
std::string command() {
  std::string line;
  if (!std::getline(std::cin, line))
    throw DriverClosed{};
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}
struct ReadyPair {
  int fd[2] = {-1, -1};
  ReadyPair() {
    require(capsid::win32::create_socket_pair(fd), "create private READY pair");
  }
  ~ReadyPair() {
    close(fd[0]);
    close(fd[1]);
  }
};
std::uint16_t read_port(int fd) {
  std::string line;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (line.find('\n') == std::string::npos) {
    require(std::chrono::steady_clock::now() < deadline, "READY deadline");
    capsid_pollfd poll{};
    poll.fd = fd;
    poll.events = POLLIN;
    const int result = capsid::win32::capsid_poll(&poll, 1, 50);
    require(result >= 0, "poll READY");
    if (!result)
      continue;
    char bytes[512];
    const auto count = capsid::win32::read_fd(fd, bytes, sizeof(bytes));
    require(count > 0, "read READY");
    line.append(bytes, static_cast<std::size_t>(count));
  }
  const auto position = line.find("\"port\":");
  require(position != std::string::npos, "READY port");
  const auto port = std::stoul(line.substr(position + 7));
  require(port > 0 && port <= 65535, "READY port range");
  return static_cast<std::uint16_t>(port);
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 3, "expected actual Worker and stop/drain case");
    const std::string scenario = argv[2];
    require(scenario == "stop" || scenario == "drain",
            "known local shutdown case");
    ReadyPair ready;
    capsid::host::StaticPoolServerOptions options;
    options.workers = 2;
    auto &worker = options.worker_options;
    worker.worker_path = argv[1];
    worker.source_bundle_path = "local-shutdown-inline";
    worker.source_name = "file://orders/v1/bundle.mjs";
    worker.application = "orders";
    worker.listen_address = "127.0.0.1";
    worker.listen_port = 0;
    worker.public_scheme = "http";
    worker.public_authority = "public.example";
    worker.request_timeout_ms = 6000;
    worker.initial_stream_window = 64U * 1024U;
    worker.strict_sandbox = false;
    worker.install_process_signals = false;
    worker.ready_fd = ready.fd[1];
    worker.cors.configured = true;
    worker.cors.allowed_origins = {"https://client.example"};
    worker.cors.allowed_methods = {"GET", "HEAD"};
    worker.cors.allowed_headers = {"x-challenge"};
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
    capsid::host::StaticPoolServer pool(std::move(options));
    std::string error;
    require(pool.start(std::vector<std::uint8_t>(source.begin(), source.end()),
                       &error),
            "start real pool: " + error);
    require(pool.active_workers() == 2, "two actual Workers at READY");
    std::cout << "{\"stage\":\"pool-ready\",\"active\":2,\"port\":"
              << read_port(ready.fd[0]) << "}" << std::endl;
    bool completed = false;
    bool requested = false;
    try {
      require(command() == "await-all-exits",
              "expected real Worker exit barrier");
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(6);
      while (pool.active_workers() &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(pool.active_workers() == 0,
              "all actual exits reached owner reactors");
      std::cout << "{\"stage\":\"no-workers-left\",\"active\":0}" << std::endl;
      require(command() == "shutdown",
              "expected accepted local requests barrier");
      for (int i = 0; i < 3; ++i) {
        if (scenario == "drain")
          pool.begin_drain(15000);
        else
          pool.request_stop();
      }
      requested = true;
      std::cout << "{\"stage\":\"shutdown-requested\",\"active\":0,\"calls\":3,"
                   "\"draining\":"
                << (pool.drain_metrics().draining ? "true" : "false") << "}"
                << std::endl;
      require(command() == "finish", "expected actual peer terminal checks");
      completed = true;
    } catch (const DriverClosed &) {
      // Preserve a client failure while normally joining the pool.
    }
    if (!completed || !requested)
      pool.request_stop();
    bool first = false, second = false;
    std::thread waiter([&] {
      std::string message;
      first = pool.wait(&message);
    });
    second = pool.wait(&error);
    waiter.join();
    require(first && second && pool.active_workers() == 0,
            "both concurrent waits complete");
    const auto metrics = pool.drain_metrics();
    require(metrics.forced_cancellations == 0, "no forced cancellations");
    std::cout
        << "{\"stage\":\"stopped\",\"active\":0,\"waiters\":2,\"draining\":"
        << (metrics.draining ? "true" : "false")
        << ",\"finished\":" << (metrics.finished ? "true" : "false")
        << ",\"forcedCancellations\":0}" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "local pool shutdown: " << error.what() << std::endl;
    return 1;
  }
}
