# A/B benchmark bodyless-ab3-20260802T154653

- commit: 530235688357d15818efbef7a83c0bacfe714778
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1560.13 | 40.244 | 47.366 | 54.322 | 0.657 | 15625 | 0 | 0 |
| candidate | 1 | 1605.57 | 39.175 | 45.230 | 53.101 | 0.636 | 16076 | 0 | 0 |
| candidate | 2 | 1595.42 | 39.441 | 45.968 | 52.433 | 0.647 | 16024 | 0 | 0 |
| baseline | 2 | 1559.67 | 40.364 | 47.281 | 58.078 | 0.658 | 15646 | 0 | 0 |
| baseline | 3 | 1517.38 | 40.593 | 48.437 | 56.834 | 0.679 | 15200 | 0 | 0 |
| candidate | 3 | 1567.15 | 40.206 | 46.521 | 54.735 | 0.655 | 15722 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1545.72
- candidate QPS: 1589.38
- delta QPS: +2.82%; delta p50: -1.96%
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): aggregate mechanism counters 234078340 → 167669113 (drop 28.37%); acceptance gate: ≥20% drop and no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 5370848 | 4549793 | +15.29% |
| client.next_event_calls | 99674562 | 68485634 | +31.29% |
| client.parsed_frames | 94303749 | 63935843 | +32.20% |
| client.parser_payload_copied_bytes | 25366596837 | 22845939609 | +9.94% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 27896400 | 24419016 | +12.47% |
| client.queued_frames | 24028979 | 21314127 | +11.30% |
| client.queued_wire_bytes | 5103782088 | 4603851432 | +9.80% |
| client.socket_read_bytes | 27631876569 | 24380401401 | +11.77% |
| client.socket_read_calls | 6129397 | 5242744 | +14.47% |
| client.socket_read_eagain | 5370811 | 4549786 | +15.29% |
| client.socket_write_bytes | 5103780144 | 4603843872 | +9.80% |
| client.socket_write_calls | 4071986 | 3767245 | +7.48% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 2016 | 1771 | +12.15% |
| host.command_batches | 14687 | 13884 | +5.47% |
| host.command_queue_hw | 112520 | 91862 | +18.36% |
| host.commands_executed | 139694 | 74152 | +46.92% |
| host.commands_submitted | 209586 | 143366 | +31.60% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 36893636310513505314 | 92233759748197182826 | -150.00% |
| host.events_queued | 272530 | 214706 | +21.22% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 69862 | 71683 | -2.61% |
| host.response_ends | 69862 | 71683 | -2.61% |
| host.response_heads | 69862 | 71683 | -2.61% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1024 | 872 | 772 | 15611 | 8269 | 26385259 | 412327 |
| baseline | worker | 0.5083 | 2664 | 2567 | 1035 | 0 | 4876944 | 0 |
| candidate | gateway | 0.1016 | 836 | 750 | 13686 | 7417 | 26246279 | 388016 |
| candidate | worker | 0.4954 | 2624 | 2542 | 1015 | 0 | 4954824 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1543.3 | 7667 | 469 | 156 | - |
| baseline | worker | 7658.1 | 752 | 112 | 626 | - |
| candidate | gateway | 1568.0 | 7850 | 641 | 158 | - |
| candidate | worker | 7646.6 | 570 | 89 | 623 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
(no symbols resolved)

### baseline-worker (perf top, self)
(no symbols resolved)

### candidate-host (perf top, self)
(no symbols resolved)

### candidate-worker (perf top, self)
(no symbols resolved)

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
