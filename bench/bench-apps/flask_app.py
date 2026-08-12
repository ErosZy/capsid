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
