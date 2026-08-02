# A/B benchmark bodyless-ab2-20260802T164047

- commit: 4d8a7831dd9df10c6de7a7f4ee872dd8ec98bdf2
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1634.44 | 38.011 | 46.872 | 53.863 | 0.627 | 16356 | 0 | 0 |
| candidate | 1 | 1650.35 | 37.651 | 46.785 | 52.061 | 0.622 | 16555 | 0 | 0 |
| candidate | 2 | 1660.25 | 37.424 | 46.070 | 56.430 | 0.618 | 16660 | 0 | 0 |
| baseline | 2 | 1636.94 | 38.000 | 47.864 | 55.371 | 0.633 | 16434 | 0 | 0 |
| baseline | 3 | 1647.59 | 37.951 | 47.052 | 53.984 | 0.629 | 16509 | 0 | 0 |
| candidate | 3 | 1653.90 | 37.906 | 45.600 | 52.732 | 0.622 | 16567 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1639.66
- candidate QPS: 1654.83
- delta QPS: +0.93%; delta p50: -0.86%
- host.commands_submitted/req: 4.4234 → 3.0004 (drop 32.17%)
- client.queued_frames/req: 1.5255 → 1.5002 (drop 1.66%)
- client.parsed_frames/req: 5.8980 → 4.5007 (drop 23.69%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 5153 | 5162 | -0.17% |
| client.next_event_calls | 90650 | 70584 | +22.14% |
| client.parsed_frames | 85497 | 65422 | +23.48% |
| client.parser_payload_copied_bytes | 22998495 | 23377175 | -1.65% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2620656 | 2604312 | +0.62% |
| client.queued_frames | 22113 | 21807 | +1.38% |
| client.queued_wire_bytes | 4634520 | 4710312 | -1.64% |
| client.socket_read_bytes | 25050423 | 24947303 | +0.41% |
| client.socket_read_calls | 5854 | 5891 | -0.63% |
| client.socket_read_eagain | 5152 | 5162 | -0.19% |
| client.socket_write_bytes | 4634520 | 4710312 | -1.64% |
| client.socket_write_calls | 3864 | 4295 | -11.15% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 618 | 571 | +7.61% |
| host.command_batches | 4789 | 4778 | +0.23% |
| host.command_queue_hw | 34497 | 27412 | +20.54% |
| host.commands_executed | 64121 | 43613 | +31.98% |
| host.commands_submitted | 64122 | 43614 | +31.98% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 68426 | 54168 | +20.84% |
| host.events_queued | 85496 | 65421 | +23.48% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21374 | 21807 | -2.03% |
| host.response_ends | 21374 | 21807 | -2.03% |
| host.response_heads | 21374 | 21807 | -2.03% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1089 | 872 | 764 | 15793 | 8253 | 25055117 | 407170 |
| baseline | worker | 0.5229 | 2592 | 2491 | 995 | 0 | 4634520 | 0 |
| candidate | gateway | 0.1083 | 868 | 773 | 15828 | 8011 | 24951986 | 377510 |
| candidate | worker | 0.5292 | 2520 | 2401 | 1044 | 0 | 4710312 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1578.1 | 8086 | 460 | 159 | - |
| baseline | worker | 7579.3 | 585 | 95 | 626 | - |
| candidate | gateway | 1574.1 | 7232 | 489 | 159 | - |
| candidate | worker | 7691.8 | 636 | 100 | 620 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
11.47%  [k] 0xffffffffb8864c81                                                                                                              
7.14%  [k] 0xffffffffb7e9212c                                                                                                               
2.81%  [k] 0xffffffffb7757919                                                                                                               
1.95%  [.] sendmsg                                                                                                                          
1.08%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
0.87%  [.] malloc                                                                                                                           
0.87%  [k] 0xffffffffb85ef09b                                                                                                               
0.87%  [k] 0xffffffffb87d52fa                                                                                                               

### baseline-worker (perf top, self)
23.16%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.90%  [.] lre_exec_backtrack                                                                                                               
4.42%  [.] malloc_usable_size                                                                                                               
4.37%  [.] cfree                                                                                                                            
4.29%  [.] malloc                                                                                                                           
3.67%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
2.12%  [.] find_own_property.lto_priv.0                                                                                                     
2.03%  [.] js_call_c_function.lto_priv.0                                                                                                    

### candidate-host (perf top, self)
10.96%  [k] 0xffffffffb8864c81                                                                                                              
7.24%  [k] 0xffffffffb7e9212c                                                                                                               
2.19%  [.] malloc                                                                                                                           
2.19%  [k] 0xffffffffb7757919                                                                                                               
1.54%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)::{lambda(boost::system::e
1.10%  [k] 0xffffffffb8865585                                                                                                               
0.88%  [.] capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)                                                               
0.88%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c

### candidate-worker (perf top, self)
23.25%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.62%  [.] lre_exec_backtrack                                                                                                               
4.88%  [.] malloc_usable_size                                                                                                               
4.61%  [.] malloc                                                                                                                           
4.53%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.53%  [.] cfree                                                                                                                            
1.87%  [.] find_own_property.lto_priv.0                                                                                                     
1.83%  [.] free_gc_object                                                                                                                   

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
