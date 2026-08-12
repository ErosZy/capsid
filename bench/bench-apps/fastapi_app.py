"""FastAPI bench app — payloads/routes mirror the capsid hono fixture and the
slim fixture (json/json16k/json64k, same pad sizes). JSON bodies are
precomputed at import; raw starlette Response avoids any re-serialization."""
import json

from fastapi import FastAPI
from starlette.responses import Response

app = FastAPI()

J = json.dumps({"status": "ok", "app": "fastapi", "item": "benchmark", "value": 42})
J16 = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 16384})
J64 = json.dumps({"status": "ok", "app": "fastapi", "pad": "x" * 65536})
B1K = "x" * 1024
B16K = "a" * 16384
B64K = "a" * 65536

_JSON_HEADERS = {"Content-Type": "application/json"}
_OCTET_HEADERS = {"Content-Type": "application/octet-stream"}


def _resp(body: str, headers: dict) -> Response:
    return Response(content=body.encode(), headers=headers)


@app.get("/@capsid/orders/bench/json")
def bench_json():
    return _resp(J, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json16k")
def bench_json16k():
    return _resp(J16, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/json64k")
def bench_json64k():
    return _resp(J64, _JSON_HEADERS)


@app.get("/@capsid/orders/bench/bytes16k")
def bench_bytes16k():
    return _resp(B16K, _OCTET_HEADERS)


@app.get("/@capsid/orders/bench/bytes64k")
def bench_bytes64k():
    return _resp(B64K, _OCTET_HEADERS)


@app.get("/@capsid/orders/fixed")
def fixed():
    return _resp(B1K, _OCTET_HEADERS)
