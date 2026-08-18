"""Flask bench app — payloads/routes mirror the hono, slim, fastapi and
sinatra fixtures (json/bytes/stream at 1k/8k/16k/32k). JSON bodies are
precomputed at import; served via flask Response without re-serialization.
bytes bodies are precomputed byte strings; stream bodies yield three chunks
with direct_passthrough so the response is genuinely chunked.

Run with gunicorn sync workers (dual-process protocol parity):
    gunicorn --workers 2 --bind 0.0.0.0:8000 flask_app:app
"""
import json

from flask import Flask, Response

app = Flask(__name__)


def _pad(size):
    return "x" * size

J1K = json.dumps({"status": "ok", "app": "flask", "pad": _pad(1024)})
J8K = json.dumps({"status": "ok", "app": "flask", "pad": _pad(8192)})
J16K = json.dumps({"status": "ok", "app": "flask", "pad": _pad(16384)})
J32K = json.dumps({"status": "ok", "app": "flask", "pad": _pad(32768)})
J64 = json.dumps({"status": "ok", "app": "flask", "pad": _pad(65536)})
B1K = "a" * 1024
B8K = "a" * 8192
B16K = "a" * 16384
B32K = "a" * 32768
FIXED = "x" * 1024  # loadgen fixed-1k expects 0x78

MATRIX_SIZES = {"1k": 1024, "4k": 4096, "8k": 8192, "16k": 16384, "32k": 32768, "64k": 65536}
MATRIX_PADS = {label: "x" * (size - 11) for label, size in MATRIX_SIZES.items()}
MATRIX_BYTES = {label: b"b" * size for label, size in MATRIX_SIZES.items()}


def _stream(size):
    third = size // 3

    def generate():
        # bytes, not str: direct_passthrough skips werkzeug's encode step
        # and the WSGI server writes the chunks as-is. Frozen stream
        # contract: b*⌊n/3⌋ c*⌊n/3⌋ d*(rest) — the loadgen verify cases
        # pin these byte positions.
        yield b"b" * third
        yield b"c" * third
        yield b"d" * (size - 2 * third)

    return Response(
        generate(),
        mimetype="application/octet-stream",
        direct_passthrough=True,
    )


@app.get("/@capsid/orders/bench/json")
def bench_json():
    return Response(J1K, mimetype="application/json")


@app.get("/@capsid/orders/bench/json8k")
def bench_json8k():
    return Response(J8K, mimetype="application/json")


@app.get("/@capsid/orders/bench/json16k")
def bench_json16k():
    return Response(J16K, mimetype="application/json")


@app.get("/@capsid/orders/bench/json32k")
def bench_json32k():
    return Response(J32K, mimetype="application/json")


@app.get("/@capsid/orders/bench/json64k")
def bench_json64k():
    return Response(J64, mimetype="application/json")


@app.get("/@capsid/orders/bench/bytes")
def bench_bytes():
    return Response(B1K, mimetype="application/octet-stream")


@app.get("/@capsid/orders/bench/bytes8k")
def bench_bytes8k():
    return Response(B8K, mimetype="application/octet-stream")


@app.get("/@capsid/orders/bench/bytes16k")
def bench_bytes16k():
    return Response(B16K, mimetype="application/octet-stream")


@app.get("/@capsid/orders/bench/bytes32k")
def bench_bytes32k():
    return Response(B32K, mimetype="application/octet-stream")


@app.get("/@capsid/orders/bench/stream")
def bench_stream():
    return _stream(1024)


@app.get("/@capsid/orders/bench/stream8k")
def bench_stream8k():
    return _stream(8192)


@app.get("/@capsid/orders/bench/stream16k")
def bench_stream16k():
    return _stream(16384)


@app.get("/@capsid/orders/bench/stream32k")
def bench_stream32k():
    return _stream(32768)


@app.get("/@capsid/orders/fixed")
def fixed():
    return Response(FIXED, mimetype="application/octet-stream")


@app.get("/@capsid/orders/bench/matrix-json-<label>")
def matrix_json(label: str):
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status=404)
    body = json.dumps({"data": MATRIX_PADS[label]}, separators=(",", ":"))
    return Response(body, mimetype="application/json", headers={"Content-Length": str(size)})


@app.get("/@capsid/orders/bench/matrix-bytes-<label>")
def matrix_bytes(label: str):
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status=404)
    return Response(MATRIX_BYTES[label], mimetype="application/octet-stream",
                    headers={"Content-Length": str(size)})


@app.get("/@capsid/orders/bench/matrix-stream-<label>")
def matrix_stream(label: str):
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status=404)

    def chunks():
        remaining = size
        chunk = b"s" * 4096
        while remaining:
            take = min(remaining, len(chunk))
            yield chunk[:take]
            remaining -= take

    return Response(chunks(), mimetype="application/octet-stream")
