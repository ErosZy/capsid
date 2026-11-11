// Transport probe: pong payload/order and interleaved data delivery.
// The observation window is ended by the driver; process death is not asserted.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "ping-flood",
        client: "raw-socket-only",
        expects: [
          "pongs that do arrive mirror the ping payload, in order",
          "interleaved data messages are delivered in full",
          "pong coalescing is allowed; the driver records the delivered count"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.binaryType = "arraybuffer";
    server.addEventListener("message", (event) => {
      server.send(event.data);
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
