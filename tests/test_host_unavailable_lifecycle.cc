// Compile the actual private implementation into this test translation unit.
// No copied Session, altered access control, or production scheduling hook.
// Owning its Asio context lets the test delay the real write completion.
#if defined(__GNUC__) && !defined(__clang__)
// GCC enables this diagnostic only when the identical .cc is included as a
// header. Its private anonymous Session member is valid in the original TU;
// suppress that one include diagnostic, leaving all other /Werror checks.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "host/static_pool_server.cc"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <iostream>
#include <sstream>

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string receive(tcp::socket &socket, bool until_close) {
  std::string bytes;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(6);
  for (;;) {
    boost::system::error_code ec;
    char chunk[4096];
    const auto count = socket.read_some(asio::buffer(chunk), ec);
    bytes.append(chunk, count);
    if (ec == asio::error::eof)
      return bytes;
    require(!ec || ec == asio::error::would_block ||
                ec == asio::error::try_again,
            "client TCP error");
    if (!until_close && !bytes.empty())
      return bytes;
    require(std::chrono::steady_clock::now() < deadline, "client TCP deadline");
    if (!count)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int exercise(const std::string &scenario) {
  const bool head = scenario == "head-close-before-completion";
  const bool close_first = scenario != "get-normal-completion";
  asio::io_context owner;
  asio::io_context client_owner;
  tcp::acceptor listener(owner, {asio::ip::address_v4::loopback(), 0});
  tcp::socket client(client_owner);
  client.connect(listener.local_endpoint());
  tcp::socket peer = listener.accept();
  listener.close();
  client.non_blocking(true);
  capsid::host::SingleWorkerServerOptions options;
  options.request_timeout_ms = 6000;
  options.write_timeout_ms = 6000;
  options.cors.configured = true;
  options.cors.allowed_origins = {"https://client.example"};
  std::size_t finished = 0;
  std::unordered_map<capsid::host::UnavailableSession *,
                     std::shared_ptr<capsid::host::UnavailableSession>>
      registry;
  auto session = std::make_shared<capsid::host::UnavailableSession>(
      std::move(peer), options, [&](capsid::host::UnavailableSession *value) {
        ++finished;
        registry.erase(value);
      });
  const std::weak_ptr<capsid::host::UnavailableSession> weak = session;
  registry.emplace(session.get(), session);
  const std::string request =
      std::string(head ? "HEAD" : "GET") +
      " /@capsid/orders/unavailable HTTP/1.1\r\nHost: public.example\r\n"
      "Origin: https://client.example\r\nConnection: close\r\n\r\n";
  // A small complete head is already in TCP before the actual read starts.
  asio::write(client, asio::buffer(request));
  session->run();
  require(owner.run_one() == 1, "actual HTTP head completion dispatched");
  require(finished == 0 && registry.size() == 1,
          "write completion remains undispatched");
  std::string wire = receive(client, false);
  require(wire.find("HTTP/1.1 503 ") == 0,
          "actual 503 bytes precede completion");
  if (close_first) {
    session->close();
    session->close();
    require(finished == 1 && registry.empty(), "local close must finish once");
  }
  session.reset();
  require(!weak.expired(),
          "queued write must retain Session after registry erasure");
  const std::size_t callbacks = owner.run();
  require(callbacks > 0, "real queued write/cancellation callbacks drained");
  require(finished == 1 && registry.empty(),
          "late completion must not finish twice");
  require(weak.expired(), "last completion must release Session");
  wire += receive(client, true);
  http::response_parser<http::string_body> parser;
  parser.eager(true);
  parser.skip(head);
  boost::system::error_code ec;
  const auto consumed = parser.put(asio::buffer(wire), ec);
  require(!ec && parser.is_done() && consumed == wire.size(),
          "complete actual HTTP response");
  const auto &reply = parser.get();
  require(reply.result() == http::status::service_unavailable,
          "wire status 503");
  require(reply[http::field::content_length] == "18", "wire content length");
  require(reply[http::field::connection] == "close", "wire connection close");
  require(reply[http::field::access_control_allow_origin] ==
              "https://client.example",
          "wire CORS");
  require(reply.body() == (head ? "" : "worker unavailable"),
          "wire method/body semantics");
  std::cout << "{\"complete\":true,\"scenario\":\"" << scenario
            << "\",\"method\":\"" << (head ? "HEAD" : "GET")
            << "\",\"wireBeforeCompletion\":true,\"closedBeforeCompletion\":"
            << (close_first ? "true" : "false")
            << ",\"queuedOwnerRetained\":true"
            << ",\"finishedCalls\":" << finished
            << ",\"remainingCallbacks\":" << callbacks
            << ",\"registryEmpty\":true,\"sessionReleased\":true,\"status\":503"
            << ",\"contentLength\":18,\"bodyBytes\":" << reply.body().size()
            << ",\"wireBytes\":" << wire.size() << ",\"tcpClosed\":true}"
            << std::endl;
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected unavailable lifecycle scenario");
    const std::string scenario = argv[1];
    require(scenario == "get-close-before-completion" ||
                scenario == "head-close-before-completion" ||
                scenario == "get-normal-completion",
            "known unavailable lifecycle scenario");
    return exercise(scenario);
  } catch (const std::exception &error) {
    std::cerr << "unavailable lifecycle: " << error.what() << std::endl;
    return 1;
  }
}
