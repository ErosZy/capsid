// Absolute contract (docs/workerd-websocket-semantics.md §1-3):
// - 101 response carries the server socket via the webSocket extension
// - every text/binary message arrives back unchanged, same type
// - close(1000, "echo-done") from the client yields a clean close event
export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "echo",
        expects: [
          "upgrade accepted with 101",
          "text message echoes as same USVString",
          "binary message echoes as same ArrayBuffer",
          "client close 1000 produces server close 1000, wasClean true"
        ]
      });
    }
    if (request.headers.get("upgrade") !== "websocket") {
      return new Response("expected websocket", { status: 400 });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.addEventListener("message", (event) => {
      server.send(event.data);
    });
    server.addEventListener("close", (event) => {
      // transport completes the handshake; nothing further to assert here
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
