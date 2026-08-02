# A/B benchmark go-vs-cpp-20260802T170344

- commit: c543834a09e6c3f675934db40be84906fe0f26ec
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: (none)
- candidate env: (none)
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1559.59 | 39.987 | 51.695 | 58.722 | 0.661 | 15643 | 0 | 0 |
| candidate | 1 | 1591.20 | 38.111 | 52.684 | 68.198 | 0.643 | 15971 | 0 | 0 |
| candidate | 2 | 1625.38 | 38.301 | 48.628 | 57.044 | 0.635 | 16303 | 0 | 0 |
| baseline | 2 | 1598.05 | 38.937 | 50.614 | 55.971 | 0.642 | 16015 | 0 | 0 |
| baseline | 3 | 1577.08 | 39.808 | 51.450 | 57.240 | 0.653 | 15830 | 0 | 0 |
| candidate | 3 | 1555.98 | 39.386 | 53.576 | 68.139 | 0.662 | 15571 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1578.24
- candidate QPS: 1590.85
- delta QPS: +0.80%; delta p50: -2.47%
- M1B acceptance gate: QPS ≥ +5% or p50 ≤ -10%; verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 0 | 4556 | +0.00% |
| client.next_event_calls | 0 | 67842 | +0.00% |
| client.parsed_frames | 0 | 63286 | +0.00% |
| client.parser_payload_copied_bytes | 0 | 22613911 | +0.00% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 0 | 2662848 | +0.00% |
| client.queued_frames | 0 | 21095 | +0.00% |
| client.queued_wire_bytes | 0 | 4556520 | +0.00% |
| client.socket_read_bytes | 0 | 24132775 | +0.00% |
| client.socket_read_calls | 0 | 5239 | +0.00% |
| client.socket_read_eagain | 0 | 4556 | +0.00% |
| client.socket_write_bytes | 0 | 4556520 | +0.00% |
| client.socket_write_calls | 0 | 3708 | +0.00% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 0 | 532 | +0.00% |
| host.command_batches | 0 | 4202 | +0.00% |
| host.command_queue_hw | 0 | 26708 | +0.00% |
| host.commands_executed | 0 | 42189 | +0.00% |
| host.commands_submitted | 0 | 42190 | +0.00% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 0 | 52532 | +0.00% |
| host.events_queued | 0 | 63285 | +0.00% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 0 | 21095 | +0.00% |
| host.response_ends | 0 | 21095 | +0.00% |
| host.response_heads | 0 | 21095 | +0.00% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1725 | -44 | -109 | 71838 | 70595 | 32212159 | 27215680 |
| baseline | worker | 0.5767 | 2504 | 2437 | 1355 | 0 | 5067576 | 0 |
| candidate | gateway | 0.1129 | 828 | 720 | 13992 | 7209 | 24136882 | 351605 |
| candidate | worker | 0.5641 | 2448 | 2330 | 963 | 0 | 4556520 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 2716.8 | 25509 | 2420 | 7889 | - |
| baseline | worker | 9080.6 | 631 | 88 | 617 | - |
| candidate | gateway | 1555.2 | 7745 | 533 | 158 | - |
| candidate | worker | 7773.1 | 798 | 117 | 601 | - |

## Dominant stacks (profile runs)


### baseline-gateway (Go pprof top)
      flat  flat%   sum%        cum   cum%
    1280ms 36.06% 36.06%     1280ms 36.06%  runtime/internal/syscall.Syscall6
     330ms  9.30% 45.35%      460ms 12.96%  runtime.cgocall
     260ms  7.32% 52.68%      260ms  7.32%  runtime.futex
      80ms  2.25% 54.93%       80ms  2.25%  runtime.memmove
      70ms  1.97% 56.90%      130ms  3.66%  runtime.scanobject
      50ms  1.41% 58.31%      300ms  8.45%  runtime.mallocgc
      50ms  1.41% 59.72%       50ms  1.41%  runtime.memclrNoHeapPointers
      40ms  1.13% 60.85%     2550ms 71.83%  net/http.(*conn).serve

### baseline-worker (perf top, self)
26.06%  [.] JS_CallInternal.lto_priv.0                                                                                                      
8.15%  [.] lre_exec_backtrack                                                                                                               
4.66%  [.] malloc_usable_size                                                                                                               
4.43%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
4.17%  [.] malloc                                                                                                                           
3.49%  [.] cfree                                                                                                                            
2.29%  [.] js_call_c_function.lto_priv.0                                                                                                    
1.88%  [.] find_own_property.lto_priv.0                                                                                                     

### candidate-host (perf top, self)
12.11%  [k] 0xffffffffb8864c81                                                                                                              
10.99%  [k] 0xffffffffb7e9212c                                                                                                              
1.57%  [.] malloc                                                                                                                           
1.35%  [k] 0xffffffffb7757919                                                                                                               
1.35%  [k] 0xffffffffb884d8b5                                                                                                               
1.12%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
1.12%  [.] cfree                                                                                                                            
0.90%  [.] decltype (((declval<boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffe

### candidate-worker (perf top, self)
25.42%  [.] JS_CallInternal.lto_priv.0                                                                                                      
10.15%  [.] lre_exec_backtrack                                                                                                              
5.12%  [.] malloc_usable_size                                                                                                               
4.47%  [.] malloc                                                                                                                           
3.78%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.31%  [.] cfree                                                                                                                            
2.11%  [.] find_own_property.lto_priv.0                                                                                                     
1.81%  [.] js_call_c_function.lto_priv.0                                                                                                    

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
