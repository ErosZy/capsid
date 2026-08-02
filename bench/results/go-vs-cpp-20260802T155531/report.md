# A/B benchmark go-vs-cpp-20260802T155531

- commit: 1b0990bcde36943116e447cac463a2a70e0572ee
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: (none)
- candidate env: (none)
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1049.19 | 52.682 | 101.005 | 186.796 | 0.979 | 10496 | 0 | 0 |
| candidate | 1 | 1296.78 | 45.757 | 69.721 | 104.016 | 0.799 | 13018 | 0 | 0 |
| candidate | 2 | 1227.81 | 45.437 | 90.624 | 187.984 | 0.846 | 12335 | 0 | 0 |
| baseline | 2 | 1466.61 | 41.138 | 54.964 | 126.494 | 0.708 | 14703 | 0 | 0 |
| baseline | 3 | 1542.10 | 40.842 | 50.235 | 56.640 | 0.680 | 15459 | 0 | 0 |
| candidate | 3 | 1252.16 | 45.938 | 80.666 | 102.543 | 0.820 | 12602 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1352.63
- candidate QPS: 1258.91
- delta QPS: -6.93%; delta p50: +1.84%
- M1B acceptance gate: QPS ≥ +5% or p50 ≤ -10%; verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 0 | 3424761 | +0.00% |
| client.next_event_calls | 0 | 49099941 | +0.00% |
| client.parsed_frames | 0 | 45675185 | +0.00% |
| client.parser_payload_copied_bytes | 0 | 16320849620 | +0.00% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 0 | 21494160 | +0.00% |
| client.queued_frames | 0 | 15227935 | +0.00% |
| client.queued_wire_bytes | 0 | 3289233960 | +0.00% |
| client.socket_read_bytes | 0 | 17417173452 | +0.00% |
| client.socket_read_calls | 0 | 3923005 | +0.00% |
| client.socket_read_eagain | 0 | 3424753 | +0.00% |
| client.socket_write_bytes | 0 | 3289233960 | +0.00% |
| client.socket_write_calls | 0 | 2851788 | +0.00% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 0 | 1557 | +0.00% |
| host.command_batches | 0 | 12230 | +0.00% |
| host.command_queue_hw | 0 | 76338 | +0.00% |
| host.commands_executed | 0 | 60713 | +0.00% |
| host.commands_submitted | 0 | 115776 | +0.00% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 0 | 12770912705899131851 | +0.00% |
| host.events_queued | 0 | 172819 | +0.00% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 0 | 57888 | +0.00% |
| host.response_ends | 0 | 57888 | +0.00% |
| host.response_heads | 0 | 57888 | +0.00% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.2079 | -5936 | -5993 | 48450 | 47636 | 21715539 | 18347942 |
| baseline | worker | 0.8146 | 2740 | 2644 | 920 | 0 | 3417488 | 0 |
| candidate | gateway | 0.1287 | 868 | 749 | 8568 | 4670 | 13895306 | 241748 |
| candidate | worker | 0.9269 | 2640 | 2524 | 623 | 0 | 2623104 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 2258.7 | 18990 | 2700 | 7234 | - |
| baseline | worker | 8851.0 | 4708 | 60 | 603 | - |
| candidate | gateway | 1076.0 | 4840 | 418 | 157 | - |
| candidate | worker | 7748.9 | 7164 | 125 | 614 | - |

## Dominant stacks (profile runs)


### baseline-gateway (Go pprof top)
      flat  flat%   sum%        cum   cum%
     740ms 26.71% 26.71%      740ms 26.71%  runtime/internal/syscall.Syscall6
     260ms  9.39% 36.10%      380ms 13.72%  runtime.cgocall
     250ms  9.03% 45.13%      250ms  9.03%  runtime.futex
      60ms  2.17% 47.29%       90ms  3.25%  runtime.mapassign_faststr
      60ms  2.17% 49.46%       60ms  2.17%  runtime.memclrNoHeapPointers
      60ms  2.17% 51.62%      130ms  4.69%  runtime.scanobject
      50ms  1.81% 53.43%       60ms  2.17%  runtime.casgstatus
      50ms  1.81% 55.23%       50ms  1.81%  runtime.nextFreeFast

### baseline-worker (perf top, self)
(no symbols resolved)

### candidate-host (perf top, self)
(no symbols resolved)

### candidate-worker (perf top, self)
(no symbols resolved)

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
