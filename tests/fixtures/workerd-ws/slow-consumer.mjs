// Absolute contract: a worker that never reads cannot push the host into
// unbounded memory — sends beyond the credit window block or error, and
// the connection stays coherent afterwards (dossier §3 bounded queuing).
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "slow-consumer",
        expects: [
          "server sends without bound",
          "worker-side buffering stays bounded (host credit window)",
          "after the client drains, the next message arrives intact"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    let n = 0;
    server.addEventListener("message", () => {});
    const timer = setInterval(() => {
      if (server.readyState === 1) server.send(String(n++));
    }, 1);
    server.addEventListener("close", () => clearInterval(timer));
    return new Response(null, { status: 101, webSocket: client });
  }
};
