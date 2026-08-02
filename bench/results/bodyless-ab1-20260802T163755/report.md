# A/B benchmark bodyless-ab1-20260802T163755

- commit: 4d8a7831dd9df10c6de7a7f4ee872dd8ec98bdf2
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1615.93 | 38.553 | 47.674 | 57.252 | 0.635 | 16239 | 0 | 0 |
| candidate | 1 | 1611.60 | 38.180 | 48.785 | 64.241 | 0.636 | 16160 | 0 | 0 |
| candidate | 2 | 1629.99 | 38.540 | 46.740 | 52.068 | 0.631 | 16353 | 0 | 0 |
| baseline | 2 | 1624.25 | 38.394 | 48.923 | 58.166 | 0.630 | 16309 | 0 | 0 |
| baseline | 3 | 1584.73 | 38.640 | 47.934 | 61.556 | 0.655 | 15899 | 0 | 0 |
| candidate | 3 | 1614.67 | 38.200 | 48.341 | 62.376 | 0.643 | 16188 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1608.30
- candidate QPS: 1618.75
- delta QPS: +0.65%; delta p50: -0.58%
- host.commands_submitted/req: 4.4376 → 2.9990 (drop 32.42%)
- client.queued_frames/req: 1.5420 → 1.4995 (drop 2.76%)
- client.parsed_frames/req: 5.9169 → 4.4985 (drop 23.97%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 4738 | 4622 | +2.45% |
| client.next_event_calls | 91255 | 70476 | +22.77% |
| client.parsed_frames | 86517 | 65854 | +23.88% |
| client.parser_payload_copied_bytes | 23272875 | 23531543 | -1.11% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2869920 | 2827224 | +1.49% |
| client.queued_frames | 22547 | 21951 | +2.64% |
| client.queued_wire_bytes | 4693896 | 4741416 | -1.01% |
| client.socket_read_bytes | 25349283 | 25112039 | +0.94% |
| client.socket_read_calls | 5442 | 5336 | +1.95% |
| client.socket_read_eagain | 4738 | 4622 | +2.45% |
| client.socket_write_bytes | 4693896 | 4741416 | -1.01% |
| client.socket_write_calls | 3552 | 3810 | -7.26% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 603 | 562 | +6.80% |
| host.command_batches | 4368 | 4250 | +2.70% |
| host.command_queue_hw | 34269 | 28279 | +17.48% |
| host.commands_executed | 64885 | 43901 | +32.34% |
| host.commands_submitted | 64887 | 43902 | +32.34% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 70093 | 54084 | +22.84% |
| host.events_queued | 86516 | 65853 | +23.88% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21629 | 21951 | -1.49% |
| host.response_ends | 21629 | 21951 | -1.49% |
| host.response_heads | 21629 | 21951 | -1.49% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1066 | 888 | 777 | 14545 | 7758 | 25353556 | 397367 |
| baseline | worker | 0.5317 | 2508 | 2413 | 1000 | 0 | 4693896 | 0 |
| candidate | gateway | 0.1061 | 908 | 806 | 14205 | 7428 | 25116194 | 371106 |
| candidate | worker | 0.5352 | 2580 | 2476 | 1007 | 0 | 4741416 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1559.3 | 7698 | 515 | 162 | - |
| baseline | worker | 7774.8 | 814 | 95 | 613 | - |
| candidate | gateway | 1553.0 | 7137 | 412 | 160 | - |
| candidate | worker | 7835.0 | 542 | 86 | 620 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
11.09%  [k] 0xffffffffb8864c81                                                                                                              
6.21%  [k] 0xffffffffb7e9212c                                                                                                               
2.00%  [.] operator new(unsigned long)                                                                                                      
1.55%  [k] 0xffffffffb7757919                                                                                                               
1.33%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
1.11%  [.] malloc                                                                                                                           
0.89%  [.] __poll                                                                                                                           
0.89%  [.] boost::asio::detail::reactive_socket_service_base::do_start_op(boost::asio::detail::reactive_socket_service_base::base_implementa

### baseline-worker (perf top, self)
24.01%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.98%  [.] lre_exec_backtrack                                                                                                               
5.28%  [.] malloc_usable_size                                                                                                               
5.06%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
4.00%  [.] malloc                                                                                                                           
2.95%  [.] cfree                                                                                                                            
1.93%  [.] find_own_property.lto_priv.0                                                                                                     
1.89%  [.] add_property.lto_priv.0                                                                                                          

### candidate-host (perf top, self)
12.64%  [k] 0xffffffffb8864c81                                                                                                              
8.58%  [k] 0xffffffffb7e9212c                                                                                                               
3.39%  [k] 0xffffffffb7757919                                                                                                               
1.58%  [.] cfree                                                                                                                            
1.35%  [.] capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)                                                               
1.13%  [.] decltype (((declval<boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffe
1.13%  [.] malloc                                                                                                                           
1.13%  [.] pthread_mutex_unlock                                                                                                             

### candidate-worker (perf top, self)
25.19%  [.] JS_CallInternal.lto_priv.0                                                                                                      
7.88%  [.] lre_exec_backtrack                                                                                                               
5.11%  [.] malloc_usable_size                                                                                                               
4.37%  [.] malloc                                                                                                                           
3.46%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.29%  [.] cfree                                                                                                                            
2.03%  [.] find_own_property.lto_priv.0                                                                                                     
1.82%  [.] js_call_c_function.lto_priv.0                                                                                                    

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
