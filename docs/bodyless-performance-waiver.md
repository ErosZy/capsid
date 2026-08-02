# Product decision waiver: bodyless performance acceptance

- Status: **waiver recorded — the mechanism ships; the performance
  acceptance gate is not met and is NOT auto-passed.**
- Date: 2026-08-03
- Commit: cffa9d2 (evidence), 15bd23a (acceptance_verdict), 813ff51
  (metrics fix), c8bf8ba (fail-closed), 52267b2 (RED test)

## What was decided

The bodyless request fusion (request-end fused into the head, no
request-direction credit, no WINDOW_UPDATE) is accepted into the M1C code
base on **correctness and mechanism** grounds. The frozen **performance**
acceptance gate was **not** met; this waiver records that explicitly instead
of treating the run as an automatic pass.

## Acceptance attempts and measured results

All runs: release build (LTO), headline zero-probe, diagnostic round-0
counters, frozen statistic (mean), frozen per-request mechanism gate
(host.commands_submitted, client.next_event_calls, client.parsed_frames,
each ÷ the same window's completed requests, all must drop ≥20%).

| gate | rule (frozen) | result |
|---|---|---|
| mechanism | 3 per-request metrics ≥20% drop | **met, exact and reproducible** — commands 3.0000→2.0000 (−33.33%), parsed 4.0000→3.0000 (−25.00%), next_event −23.7..24.2%, identical across 5 independent runs (3 saturated + 2 single-connection) |
| saturated QPS (64 conn/64 inflight) | per-run delta_qps ≥ 0 | **not met** — ab1 −3.07%, ab2 −0.63%, ab3 +1.85% (pooled −0.59%); the negatives are machine-drift artifacts (ab1's own baseline spanned 1424–1644 QPS across rounds) but the per-run rule is the per-run rule |
| latency branch (1 conn/1 inflight) | p50 improvement ≥10% | **not met** — two independent runs: p50 −2.03% (1.44→1.41 ms) and −3.62% (1.46→1.40 ms); the fusion saves 30–60 µs/request at ~1.4 ms unloaded latency |

## Why the mechanism still ships

1. The per-request mechanism drop is exact and reproducible: every bodyless
   request removes exactly one host→worker command, one worker→host event
   frame and ~one event poll. This is the designed effect of the fusion.
2. The saturated QPS difference is inside the run-to-run machine noise band
   (a single run's baseline varies 13%+); there is no consistent direction
   (pooled −0.59%, and the single-connection runs show the opposite sign:
   QPS +2.15%/+6.60%).
3. The unloaded latency improves in both directions (p50, p95, p99 all
   lower on the fused side in both single-connection runs) — the fusion is
   not a latency regression at any percentile in the measured data.
4. p99 at saturation (pooled +13.2% in the saturated runs) remains an open
   tail-latency question and is tracked, not waived away: the 1/1 runs show
   p99 *improving* (−2.5..−10.7%), which suggests the saturated p99 delta is
   queueing-dominated rather than a per-request regression.

## What is NOT claimed

- The bodyless A/B does not claim to have passed its performance
  acceptance gate. `bench/results/*/manifest.json` records
  `acceptance_verdict` independently of `evidence_status` (complete
  evidence ≠ PASS).
- The Go/C++ comparison (QPS parity, C++ p99 +12.47% worse) is a separate,
  still-open performance item (M1B FAIL).

## Revisit conditions

- A dedicated stable machine / CI benchmark runner that removes the
  host-machine drift from the saturated measurements; or
- the response-side fusion + event batching work (the same mechanism on the
  worker→host direction), which is the next performance lever and would
  re-open both the saturated QPS and the latency acceptance; or
- a request from the product side to re-benchmark on a target deployment.
