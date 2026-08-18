"""FastAPI bench app — payloads/routes mirror the capsid hono, slim and flask
fixtures (json/bytes/stream at 1k/4k/8k/16k/32k/64k). JSON bodies are
precomputed at import; raw starlette Response avoids any re-serialization.
bytes bodies are precomputed byte strings; stream bodies yield 4 KiB chunks
of 's' with the frozen stream contract so loadgen verify matches all stacks."""
import json

from fastapi import FastAPI
from starlette.responses import Response, StreamingResponse

app = FastAPI()

J1K = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 1024})
J8K = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 8192})
J16K = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 16384})
J32K = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 32768})
J64 = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 65536})
B1K = "a" * 1024  # loadgen legacy bytes expects 0x61
B8K = "a" * 8192
B16K = "a" * 16384
B32K = "a" * 32768
FIXED = "x" * 1024  # loadgen fixed-1k expects 0x78

MATRIX_SIZES = {"1k": 1024, "4k": 4096, "8k": 8192, "16k": 16384, "32k": 32768, "64k": 65536}
MATRIX_PADS = {label: "x" * (size - 11) for label, size in MATRIX_SIZES.items()}
MATRIX_BYTES = {label: b"b" * size for label, size in MATRIX_SIZES.items()}

_JSON_HEADERS = {"Content-Type": "application/json"}
_OCTET_HEADERS = {"Content-Type": "application/octet-stream"}


def _resp(body: str, headers: dict) -> Response:
    return Response(content=body.encode(), headers=headers)


@app.get("/@capsid/orders/bench/json")
def bench_json():
    return _resp(J1K, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json8k")
def bench_json8k():
    return _resp(J8K, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json16k")
def bench_json16k():
    return _resp(J16K, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json32k")
def bench_json32k():
    return _resp(J32K, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json64k")
def bench_json64k():
    return _resp(J64, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/bytes")
def bench_bytes():
    return _resp(B1K, _OCTET_HEADERS)


@app.get("/@capsid/orders/bench/bytes8k")
def bench_bytes8k():
    return _resp(B8K, _OCTET_HEADERS)


@app.get("/@capsid/orders/bench/bytes16k")
def bench_bytes16k():
    return _resp(B16K, _OCTET_HEADERS)


@app.get("/@capsid/orders/bench/bytes32k")
def bench_bytes32k():
    return _resp(B32K, _OCTET_HEADERS)


@app.get("/@capsid/orders/fixed")
def fixed():
    return _resp(FIXED, _OCTET_HEADERS)


def _stream(size: int) -> StreamingResponse:
    # Frozen stream contract: b*⌊n/3⌋ c*⌊n/3⌋ d*(rest) — the loadgen
    # verify cases pin these byte positions across all stacks.
    third = size // 3

    def generate():
        yield b"b" * third
        yield b"c" * third
        yield b"d" * (size - 2 * third)

    return StreamingResponse(generate(), media_type="application/octet-stream")


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


def _matrix_json(label: str) -> Response:
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status_code=404)
    # Compact JSON identical to the other stacks' matrix-json payload.
    body = json.dumps({"data": MATRIX_PADS[label]}, separators=(",", ":"))
    return Response(content=body.encode(), headers={**_JSON_HEADERS, "Content-Length": str(size)})


def _matrix_bytes(label: str) -> Response:
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status_code=404)
    return Response(content=MATRIX_BYTES[label], headers={**_OCTET_HEADERS, "Content-Length": str(size)})


def _matrix_stream(label: str) -> StreamingResponse:
    size = MATRIX_SIZES.get(label)
    if size is None:
        return Response("not found", status_code=404)
    chunk = b"s" * 4096

    def chunks():
        remaining = size
        while remaining:
            take = min(remaining, len(chunk))
            yield chunk[:take]
            remaining -= take

    return StreamingResponse(chunks(), media_type="application/octet-stream")


for _label in MATRIX_SIZES:
    app.add_api_route(f"/@capsid/orders/bench/matrix-json-{_label}",
                      (lambda label: lambda: _matrix_json(label))(_label), methods=["GET"])
    app.add_api_route(f"/@capsid/orders/bench/matrix-bytes-{_label}",
                      (lambda label: lambda: _matrix_bytes(label))(_label), methods=["GET"])
    app.add_api_route(f"/@capsid/orders/bench/matrix-stream-{_label}",
                      (lambda label: lambda: _matrix_stream(label))(_label), methods=["GET"])
