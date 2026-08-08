# Sinatra bench app — payloads/routes mirror the capsid hono and slim fixtures.
# JSON bodies precomputed at boot; served as plain strings.
require "sinatra/base"
require "json"

class BenchApp < Sinatra::Base
  J  = JSON.generate(status: "ok", app: "sinatra", item: "benchmark", value: 42)
  J16 = JSON.generate(status: "ok", app: "sinatra", pad: "x" * 16384)
  J64 = JSON.generate(status: "ok", app: "sinatra", pad: "x" * 65536)
  B1K = "x" * 1024
  B16K = "a" * 16384
  B64K = "a" * 65536

  get "/@capsid/orders/bench/json" do
    content_type "application/json"
    J
  end

  get "/@capsid/orders/bench/json16k" do
    content_type "application/json"
    J16
  end

  get "/@capsid/orders/bench/json64k" do
    content_type "application/json"
    J64
  end

  get "/@capsid/orders/bench/bytes16k" do
    content_type "application/octet-stream"
    B16K
  end

  get "/@capsid/orders/bench/bytes64k" do
    content_type "application/octet-stream"
    B64K
  end

  get "/@capsid/orders/fixed" do
    content_type "application/octet-stream"
    B1K
  end
end
