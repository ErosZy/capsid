// Absolute contract (dossier §3 as corrected 2026-10-12): workerd has NO
// messageerror event, and invalid UTF-8 in a text frame is DELIVERED to
// the worker as a lossily-decoded string (U+FFFD per invalid sequence,
// here fffd,fffd,fffd,62) with the connection remaining open. Driving
// this scenario requires a raw-socket client (the native client
// validates UTF-8 before sending); the runner marks it raw-only.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "invalid-utf8",
        client: "raw-socket-only",
        expects: [
          "malformed text frame is delivered as a message event",
          "payload decodes lossily to U+FFFD replacement characters",
          "the connection stays open afterwards"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.binaryType = "arraybuffer";
    server.addEventListener("message", (event) => {
      if (typeof event.data === "string") {
        const cps = [...event.data]
          .map((ch) => ch.codePointAt(0).toString(16))
          .join(",");
        server.send("cps:" + cps);
      } else {
        server.send(event.data);
      }
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
