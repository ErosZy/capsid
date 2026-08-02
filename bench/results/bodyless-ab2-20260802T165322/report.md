# A/B benchmark bodyless-ab2-20260802T165322

- commit: 2637e8dee16c5e559dd412815a9bb41f1885bd86
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1675.73 | 37.243 | 44.496 | 49.088 | 0.618 | 16807 | 0 | 0 |
| candidate | 1 | 1658.60 | 37.578 | 46.150 | 54.705 | 0.628 | 16612 | 0 | 0 |
| candidate | 2 | 1626.76 | 38.239 | 48.005 | 58.257 | 0.630 | 16291 | 0 | 0 |
| baseline | 2 | 1625.51 | 38.407 | 46.562 | 53.051 | 0.633 | 16294 | 0 | 0 |
| baseline | 3 | 1644.39 | 38.033 | 46.573 | 52.879 | 0.626 | 16507 | 0 | 0 |
| candidate | 3 | 1629.05 | 38.383 | 47.551 | 55.825 | 0.632 | 16297 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1648.54
- candidate QPS: 1638.14
- delta QPS: -0.63%; delta p50: +0.45%
- host.commands_submitted/req: 3.0000 → 2.0000 (drop 33.33%)
- client.next_event_calls/req: 4.2144 → 3.2163 (drop 23.68%)
- client.parsed_frames/req: 4.0000 → 3.0000 (drop 25.00%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 4583 | 4719 | -2.97% |
| client.next_event_calls | 90104 | 70195 | +22.10% |
| client.parsed_frames | 85521 | 65476 | +23.44% |
| client.parser_payload_copied_bytes | 23004951 | 23396471 | -1.70% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2811792 | 2733696 | +2.78% |
| client.queued_frames | 22065 | 21825 | +1.09% |
| client.queued_wire_bytes | 4634520 | 4714200 | -1.72% |
| client.socket_read_bytes | 25057455 | 24967895 | +0.36% |
| client.socket_read_calls | 5283 | 5428 | -2.74% |
| client.socket_read_eagain | 4583 | 4719 | -2.97% |
| client.socket_write_bytes | 4634520 | 4714200 | -1.72% |
| client.socket_write_calls | 3432 | 3908 | -13.87% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 599 | 537 | +10.35% |
| host.command_batches | 4224 | 4347 | -2.91% |
| host.command_queue_hw | 34513 | 27324 | +20.83% |
| host.commands_executed | 64139 | 43649 | +31.95% |
| host.commands_submitted | 64140 | 43650 | +31.95% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 67848 | 54949 | +19.01% |
| host.events_queued | 85520 | 65475 | +23.44% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21380 | 21825 | -2.08% |
| host.response_ends | 21380 | 21825 | -2.08% |
| host.response_heads | 21380 | 21825 | -2.08% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1103 | 816 | 719 | 14087 | 7588 | 25061584 | 394517 |
| baseline | worker | 0.5328 | 2592 | 2492 | 1003 | 0 | 4634520 | 0 |
| candidate | gateway | 0.1047 | 908 | 807 | 14491 | 7395 | 24972147 | 355445 |
| candidate | worker | 0.5265 | 2464 | 2370 | 1004 | 0 | 4714200 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1586.9 | 7799 | 640 | 159 | - |
| baseline | worker | 7662.2 | 630 | 103 | 626 | - |
| candidate | gateway | 1544.1 | 7406 | 561 | 160 | - |
| candidate | worker | 7765.7 | 595 | 99 | 618 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
12.53%  [k] 0xffffffffb8864c81                                                                                                              
6.48%  [k] 0xffffffffb7e9212c                                                                                                               
2.59%  [k] 0xffffffffb7757919                                                                                                               
2.38%  [.] sendmsg                                                                                                                          
1.51%  [k] 0xffffffffb7acd546                                                                                                               
1.51%  [k] 0xffffffffb884d8b5                                                                                                               
1.30%  [.] decltype (((declval<boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffe
1.30%  [.] pthread_mutex_lock                                                                                                               

### baseline-worker (perf top, self)
23.76%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.46%  [.] lre_exec_backtrack                                                                                                               
5.10%  [.] malloc_usable_size                                                                                                               
4.40%  [.] malloc                                                                                                                           
3.39%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.21%  [.] cfree                                                                                                                            
1.94%  [.] find_own_property.lto_priv.0                                                                                                     
1.85%  [.] js_call_c_function.lto_priv.0                                                                                                    

### candidate-host (perf top, self)
9.19%  [k] 0xffffffffb7e9212c                                                                                                               
8.07%  [k] 0xffffffffb8864c81                                                                                                               
2.02%  [k] 0xffffffffb7757919                                                                                                               
1.35%  [k] 0xffffffffb884d8b5                                                                                                               
1.12%  [.] malloc                                                                                                                           
0.90%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
0.90%  [.] cfree                                                                                                                            
0.90%  [.] decltype (((declval<boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffe

### candidate-worker (perf top, self)
24.68%  [.] JS_CallInternal.lto_priv.0                                                                                                      
8.39%  [.] lre_exec_backtrack                                                                                                               
4.95%  [.] malloc_usable_size                                                                                                               
4.26%  [.] malloc                                                                                                                           
4.22%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.91%  [.] cfree                                                                                                                            
1.96%  [.] free_gc_object                                                                                                                   
1.78%  [.] JS_FreeValue                                                                                                                     

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
