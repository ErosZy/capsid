# Gzip in an HTTP upgrade fallback response

A fetch request with `Upgrade: websocket` can receive an ordinary HTTP response.
Its body uses the existing credit-controlled native channel and its single
transport owner. A gzip response is decoded before its bytes reach the Fetch
body reader. The encoded HTTP headers remain available on the response.

The decoder uses the same native gzip parser and allocator as ordinary fetch.
It is CPU-only and creates no second HTTP client or connection. It writes only
into the current reader's buffer, in steps of at most 4,096 decoded bytes, and
returns channel credit for encoded input actually consumed. Pending data stays
inside the channel window. Cancellation, parser failure and EOF dispose the
decoder and follow the existing channel cancellation and retirement paths.
The existing 101 WebSocket path and raw response body retain their behavior.

An enabled response body limit counts decoded bytes. An encoded Content-Length
does not reject an otherwise valid decoded body. The C ABI limit fixture sends
real stored DEFLATE blocks with a gzip header, CRC32 and ISIZE. An independent
zlib decoder verifies the complete source vectors and rejects altered CRC and
missing trailers. Full-body fragmented cases wait for the previous fragment's
actual native credit before producing the next fragment. Consumer cancellation
keeps the trailer and EOF withheld. Raw source cases and their budgets remain.

The DefaultReader gates use header readiness followed by 69 consumer read
acknowledgments for 274,449 bytes. The source holds EOF until the final read.
Raw and gzip streaming and cancellation are compared with the same locked
primary program, including full backing hashes and reader closed/release state.
A real whole-body wait cannot acknowledge the first held part and must trigger
the two second source gate. Each positive run also checks source socket closure,
a healthy subsequent request and a natural native process exit. Ordinary fetch
and Upgrade fallback have separate required registrations. Their independent
report contract rejects omitted cases, weaker deadlines, changed programs,
changed public state, early source EOF, leaked sockets and forced cleanup.

These checks establish functional behavior. They do not replace final combined
acceptance, sanitizer findings, resource soak or formal performance qualification.
