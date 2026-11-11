// Ordinary repeated accept is a no-op; the first acceptance remains active.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "double-accept",
        expects: [
          "second accept() is a silent no-op (no throw, first accept stands)",
          "socket continues to answer afterwards"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    let doubleAcceptThrew = null;
    try {
      server.accept();
    } catch (error) {
      doubleAcceptThrew = error?.name ?? String(error);
    }
    server.binaryType = "arraybuffer";
    let firstMessage = true;
    server.addEventListener("message", (event) => {
      if (firstMessage) {
        firstMessage = false;
        server.send(`double-accept:${doubleAcceptThrew}`);
        return;
      }
      server.send(event.data);
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
