// Absolute contract (dossier §4): the server may close with code+reason;
// the client observes code 3000 and reason "planned", and close() with a
// reserved code (1005) throws DOMInvalidAccessError before any wire IO.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "server-close",
        expects: [
          "server close(3000, 'planned') observed by client as code 3000",
          "reason preserved as 'planned'",
          "close(1005) throws InvalidAccessError synchronously"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.close(3000, "planned");
    return new Response(null, { status: 101, webSocket: client });
  }
};
