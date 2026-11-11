// Absolute contract (dossier close-code matrix): close() codes outside
// 1000-4999 (minus the reserved 1004/1005/1006/1015 in legacy mode)
// throw DOMException InvalidAccessError; a close issued from the server
// with a valid code reaches the client intact; an out-of-range close
// from the CLIENT is checked on a separate path. The pinned reference's
// automatic reply echoes 999; Capsid deliberately validates it as RFC 1002.
// Separating paths removes the race with the server's planned 4321 Close.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "close-boundary",
        expects: [
          "server close(4321, 'edge') delivers 4321 to the client",
          "close(999) from the client fails the connection (client sees 1006)"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    let rejectedClose = null;
    try {
      server.close(999, "invalid");
    } catch (error) {
      rejectedClose = error?.name ?? String(error);
    }
    server.addEventListener("message", (event) => {
      // Report what the invalid close() attempt did, then close clean
      // on the worker's own terms.
      server.send(`close-999-threw:${rejectedClose}`);
      if (url.pathname !== '/invalid-client-close') server.close(4321, "edge");
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};
