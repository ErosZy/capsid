# A/B benchmark bodyless-ab1-20260802T165033

- commit: 2637e8dee16c5e559dd412815a9bb41f1885bd86
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1424.05 | 41.905 | 62.050 | 79.013 | 0.718 | 14257 | 0 | 0 |
| candidate | 1 | 1462.46 | 39.301 | 66.092 | 115.586 | 0.718 | 14630 | 0 | 0 |
| candidate | 2 | 1460.26 | 40.163 | 60.617 | 95.584 | 0.715 | 14615 | 0 | 0 |
| baseline | 2 | 1630.92 | 37.900 | 47.883 | 62.232 | 0.639 | 16368 | 0 | 0 |
| baseline | 3 | 1644.02 | 37.983 | 46.252 | 53.792 | 0.627 | 16495 | 0 | 0 |
| candidate | 3 | 1631.86 | 38.401 | 46.752 | 53.651 | 0.633 | 16362 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1566.33
- candidate QPS: 1518.19
- delta QPS: -3.07%; delta p50: +0.07%
- host.commands_submitted/req: 3.0000 → 2.0000 (drop 33.33%)
- client.next_event_calls/req: 4.2305 → 3.2262 (drop 23.74%)
- client.parsed_frames/req: 4.0000 → 3.0000 (drop 25.00%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 4964 | 4860 | +2.10% |
| client.next_event_calls | 91141 | 69325 | +23.94% |
| client.parsed_frames | 86177 | 64465 | +25.19% |
| client.parser_payload_copied_bytes | 23181415 | 23035207 | +0.63% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2753280 | 2605176 | +5.38% |
| client.queued_frames | 22713 | 21488 | +5.39% |
| client.queued_wire_bytes | 4681560 | 4641408 | +0.86% |
| client.socket_read_bytes | 25249663 | 24582367 | +2.64% |
| client.socket_read_calls | 5663 | 5559 | +1.84% |
| client.socket_read_eagain | 4964 | 4860 | +2.10% |
| client.socket_write_bytes | 4681560 | 4641408 | +0.86% |
| client.socket_write_calls | 3745 | 4060 | -8.41% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 617 | 541 | +12.32% |
| host.command_batches | 4595 | 4493 | +2.22% |
| host.command_queue_hw | 34973 | 27170 | +22.31% |
| host.commands_executed | 64630 | 42975 | +33.51% |
| host.commands_submitted | 64632 | 42976 | +33.51% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 69202 | 53770 | +22.30% |
| host.events_queued | 86176 | 64464 | +25.19% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21544 | 21488 | +0.26% |
| host.response_ends | 21544 | 21488 | +0.26% |
| host.response_heads | 21544 | 21488 | +0.26% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1075 | 848 | 740 | 15219 | 8057 | 25254163 | 406600 |
| baseline | worker | 0.5288 | 2468 | 2377 | 1006 | 0 | 4681560 | 0 |
| candidate | gateway | 0.1072 | 828 | 725 | 14909 | 7556 | 24586765 | 357952 |
| candidate | worker | 0.5334 | 2560 | 2458 | 997 | 0 | 4641408 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1564.8 | 7820 | 466 | 161 | - |
| baseline | worker | 7695.7 | 1025 | 81 | 622 | - |
| candidate | gateway | 1547.4 | 7329 | 527 | 157 | - |
| candidate | worker | 7702.9 | 593 | 100 | 620 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
10.17%  [k] 0xffffffffb8864c81                                                                                                              
8.23%  [k] 0xffffffffb7e9212c                                                                                                               
2.60%  [.] malloc                                                                                                                           
2.38%  [k] 0xffffffffb7757919                                                                                                               
1.30%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)::{lambda(boost::system::e
1.08%  [.] operator new(unsigned long)                                                                                                      
0.87%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::write_body_block(unsigned long, std::vector<unsigned char, std::allocato
0.87%  [.] sendmsg                                                                                                                          

### baseline-worker (perf top, self)
25.13%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.48%  [.] lre_exec_backtrack                                                                                                               
4.94%  [.] malloc_usable_size                                                                                                               
3.89%  [.] malloc                                                                                                                           
3.32%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
2.97%  [.] cfree                                                                                                                            
2.23%  [.] find_own_property.lto_priv.0                                                                                                     
1.92%  [.] js_call_c_function.lto_priv.0                                                                                                    

### candidate-host (perf top, self)
11.92%  [k] 0xffffffffb8864c81                                                                                                              
7.51%  [k] 0xffffffffb7e9212c                                                                                                               
1.99%  [.] malloc                                                                                                                           
1.55%  [k] 0xffffffffb7757919                                                                                                               
1.32%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
1.10%  [.] capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)                                                               
0.88%  [.] boost::beast::http::detail::basic_parser_base::parse_field(char const*&, char const*, boost::core::basic_string_view<char>&, boos
0.88%  [.] capsid_worker_next_event                                                                                                         

### candidate-worker (perf top, self)
24.68%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.17%  [.] lre_exec_backtrack                                                                                                               
5.20%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
4.98%  [.] malloc_usable_size                                                                                                               
3.76%  [.] malloc                                                                                                                           
3.41%  [.] cfree                                                                                                                            
2.01%  [.] find_own_property.lto_priv.0                                                                                                     
1.88%  [.] free_gc_object                                                                                                                   

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
