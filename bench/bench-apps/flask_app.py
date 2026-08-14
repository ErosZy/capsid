"""Flask bench app — payloads/routes mirror the hono, slim, fastapi and
sinatra fixtures (json/json16k/json64k, same pad sizes). JSON bodies are
precomputed at import; served via flask Response without re-serialization.

Run with gunicorn sync workers (dual-process protocol parity):
    gunicorn --workers 2 --bind 0.0.0.0:8000 flask_app:app
"""
import json

from flask import Flask, Response

app = Flask(__name__)

J = json.dumps({"status": "ok", "app": "flask", "item": "benchmark", "value": 42})
J16 = json.dumps({"status": "ok", "app": "flask", "pad": "x" * 16384})
J64 = json.dumps({"status": "ok", "app": "flask", "pad": "x" * 65536})
B1K = "x" * 1024
MATRIX_SIZES = {"1k": 1024, "4k": 4096, "16k": 16384, "32k": 32768, "64k": 65536}
MATRIX_PADS = {label: "x" * (size - 11) for label, size in MATRIX_SIZES.items()}
MATRIX_BYTES = {label: b"b" * size for label, size in MATRIX_SIZES.items()}


@app.get("/@capsid/orders/bench/json")
def bench_json():
    return Response(J, mimetype="application/json")


@app.get("/@capsid/orders/bench/json16k")
def bench_json16k():
    return Response(J16, mimetype="application/json")


@app.get("/@capsid/orders/bench/json64k")
def bench_json64k():
    return Response(J64, mimetype="application/json")


@app.get("/@capsid/orders/fixed")
def fixed():
    return Response(B1K, mimetype="application/octet-stream")


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
