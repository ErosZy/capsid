# A/B benchmark bodyless-ab1-20260802T154053

- commit: 530235688357d15818efbef7a83c0bacfe714778
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1480.23 | 41.791 | 55.048 | 63.806 | 0.697 | 14811 | 0 | 0 |
| candidate | 1 | 1535.87 | 40.542 | 51.718 | 61.570 | 0.669 | 15399 | 0 | 0 |
| candidate | 2 | 1564.38 | 40.134 | 47.121 | 55.159 | 0.657 | 15692 | 0 | 0 |
| baseline | 2 | 1550.73 | 40.305 | 48.401 | 52.291 | 0.662 | 15571 | 0 | 0 |
| baseline | 3 | 1501.30 | 40.959 | 52.706 | 61.539 | 0.688 | 15062 | 0 | 0 |
| candidate | 3 | 1558.17 | 40.579 | 46.767 | 57.351 | 0.661 | 15646 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1510.75
- candidate QPS: 1552.81
- delta QPS: +2.78%; delta p50: -1.46%
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): aggregate mechanism counters 220126736 → 158905565 (drop 27.81%); acceptance gate: ≥20% drop and no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 5090831 | 4427723 | +13.03% |
| client.next_event_calls | 93680360 | 64817974 | +30.81% |
| client.parsed_frames | 88589561 | 60390254 | +31.83% |
| client.parser_payload_copied_bytes | 23829482442 | 21579002008 | +9.44% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 26836488 | 23695200 | +11.71% |
| client.queued_frames | 22605529 | 20131933 | +10.94% |
| client.queued_wire_bytes | 4795402776 | 4348497528 | +9.32% |
| client.socket_read_bytes | 25957377538 | 23028438712 | +11.28% |
| client.socket_read_calls | 5804726 | 5079511 | +12.49% |
| client.socket_read_eagain | 5090799 | 4427714 | +13.03% |
| client.socket_write_bytes | 4795402776 | 4348497528 | +9.32% |
| client.socket_write_calls | 3871457 | 3692315 | +4.63% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 1935 | 1716 | +11.32% |
| host.command_batches | 14551 | 13959 | +4.07% |
| host.command_queue_hw | 108686 | 89622 | +17.54% |
| host.commands_executed | 134940 | 72657 | +46.16% |
| host.commands_submitted | 203142 | 140268 | +30.95% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 36893596150950175307 | 18446859793200668119 | +50.00% |
| host.events_queued | 264644 | 209912 | +20.68% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 67714 | 70134 | -3.57% |
| host.response_ends | 67714 | 70134 | -3.57% |
| host.response_heads | 67714 | 70134 | -3.57% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1020 | 908 | 801 | 15658 | 8179 | 26539981 | 400225 |
| baseline | worker | 0.5066 | 2652 | 2555 | 1015 | 0 | 4902432 | 0 |
| candidate | gateway | 0.1000 | 852 | 756 | 14170 | 7499 | 26110309 | 378555 |
| candidate | worker | 0.4996 | 2704 | 2608 | 1010 | 0 | 4929120 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1549.0 | 8260 | 568 | 162 | - |
| baseline | worker | 7692.5 | 603 | 102 | 615 | - |
| candidate | gateway | 1557.2 | 7402 | 524 | 160 | - |
| candidate | worker | 7781.1 | 602 | 111 | 615 | - |

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
