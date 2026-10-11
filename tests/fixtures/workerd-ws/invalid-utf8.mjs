// Absolute contract (dossier §3, §5): a text frame with invalid UTF-8
// surfaces as messageerror on the receiving side (never a message), and
// the connection's subsequent behavior stays deterministic.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "invalid-utf8",
        expects: [
          "invalid UTF-8 text frame yields messageerror, not message",
          "a following valid binary frame still arrives"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    let sawError = false;
    server.addEventListener("messageerror", () => { sawError = true; });
    server.addEventListener("message", (event) => {
      if (typeof event.data === "string" && event.data === "state?") {
        server.send(sawError ? "messageerror-seen" : "no-error");
      } else if (event.data instanceof ArrayBuffer) {
        server.send(new Uint8Array([1]));
      }
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
