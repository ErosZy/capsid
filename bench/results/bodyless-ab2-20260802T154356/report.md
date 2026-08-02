# A/B benchmark bodyless-ab2-20260802T154356

- commit: 530235688357d15818efbef7a83c0bacfe714778
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1441.87 | 41.281 | 66.064 | 98.505 | 0.708 | 14428 | 0 | 0 |
| candidate | 1 | 1578.36 | 39.653 | 48.381 | 56.507 | 0.647 | 15828 | 0 | 0 |
| candidate | 2 | 1512.76 | 39.833 | 52.249 | 114.129 | 0.674 | 15174 | 0 | 0 |
| baseline | 2 | 1438.22 | 42.298 | 57.375 | 82.311 | 0.720 | 14419 | 0 | 0 |
| baseline | 3 | 1518.21 | 41.394 | 48.919 | 58.786 | 0.674 | 15246 | 0 | 0 |
| candidate | 3 | 1562.50 | 40.087 | 48.160 | 55.614 | 0.655 | 15646 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1466.10
- candidate QPS: 1551.21
- delta QPS: +5.81%; delta p50: -4.32%
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): aggregate mechanism counters 212169994 → 159698995 (drop 24.73%); acceptance gate: ≥20% drop and no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 4856854 | 4419680 | +9.00% |
| client.next_event_calls | 90296220 | 65163103 | +27.83% |
| client.parsed_frames | 85439405 | 60743432 | +28.90% |
| client.parser_payload_copied_bytes | 22982140856 | 21705238082 | +5.56% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 26433648 | 23796720 | +9.98% |
| client.queued_frames | 21923416 | 20249320 | +7.64% |
| client.queued_wire_bytes | 4627784064 | 4373853120 | +5.49% |
| client.socket_read_bytes | 25034990204 | 23163156722 | +7.48% |
| client.socket_read_calls | 5547320 | 5073536 | +8.54% |
| client.socket_read_eagain | 4856811 | 4419669 | +9.00% |
| client.socket_write_bytes | 4627784064 | 4373851392 | +5.49% |
| client.socket_write_calls | 3632111 | 3685115 | -1.46% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 1910 | 1723 | +9.79% |
| host.command_batches | 13821 | 13386 | +3.15% |
| host.command_queue_hw | 107549 | 89701 | +16.60% |
| host.commands_executed | 133483 | 71848 | +46.17% |
| host.commands_submitted | 200367 | 140090 | +30.08% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 35474603569776951265 | 95071710896810638629 | -168.00% |
| host.events_queued | 258570 | 209610 | +18.93% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 66789 | 70045 | -4.88% |
| host.response_ends | 66789 | 70045 | -4.88% |
| host.response_heads | 66789 | 70045 | -4.88% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1001 | 904 | 800 | 15221 | 8312 | 26517564 | 432036 |
| baseline | worker | 0.5022 | 2668 | 2562 | 1000 | 0 | 4899552 | 0 |
| candidate | gateway | 0.1018 | 900 | 787 | 14062 | 7652 | 26198346 | 401628 |
| candidate | worker | 0.5060 | 2644 | 2547 | 1039 | 0 | 4945752 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1520.8 | 7990 | 594 | 160 | - |
| baseline | worker | 7628.4 | 705 | 95 | 624 | - |
| candidate | gateway | 1554.1 | 7419 | 526 | 158 | - |
| candidate | worker | 7724.6 | 582 | 102 | 622 | - |

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
