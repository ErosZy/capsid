// Absolute contract: WebSocket messages are delivered whole regardless of
// how the peer fragments writes: 100 single-byte client writes surface as
// exactly one 100-byte message, byte-identical.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "fragmented",
        expects: [
          "100 fragmented writes arrive as one 100-byte binary message",
          "content matches the sent bytes exactly"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    // Byte-exact binary contract requires the ArrayBuffer view (the
    // reference default binaryType is "blob").
    server.binaryType = "arraybuffer";
    server.addEventListener("message", (event) => {
      server.send(new Uint8Array([event.data.byteLength & 0xff]));
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
