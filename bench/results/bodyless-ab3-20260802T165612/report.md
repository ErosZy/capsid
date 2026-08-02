# A/B benchmark bodyless-ab3-20260802T165612

- commit: 2637e8dee16c5e559dd412815a9bb41f1885bd86
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1624.37 | 38.114 | 48.610 | 61.692 | 0.630 | 16262 | 0 | 0 |
| candidate | 1 | 1629.80 | 37.837 | 49.574 | 61.754 | 0.635 | 16330 | 0 | 0 |
| candidate | 2 | 1655.03 | 37.643 | 46.194 | 53.788 | 0.620 | 16556 | 0 | 0 |
| baseline | 2 | 1624.10 | 38.491 | 48.108 | 53.259 | 0.630 | 16263 | 0 | 0 |
| baseline | 3 | 1593.96 | 38.594 | 49.958 | 67.839 | 0.654 | 15958 | 0 | 0 |
| candidate | 3 | 1647.39 | 37.960 | 45.921 | 54.011 | 0.627 | 16487 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1614.14
- candidate QPS: 1644.07
- delta QPS: +1.85%; delta p50: -1.53%
- host.commands_submitted/req: 3.0000 → 2.0000 (drop 33.33%)
- client.next_event_calls/req: 4.2378 → 3.2142 (drop 24.15%)
- client.parsed_frames/req: 4.0000 → 3.0000 (drop 25.00%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 5204 | 4781 | +8.13% |
| client.next_event_calls | 92773 | 71751 | +22.66% |
| client.parsed_frames | 87569 | 66970 | +23.52% |
| client.parser_payload_copied_bytes | 23555863 | 23930327 | -1.59% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2759232 | 2860272 | -3.66% |
| client.queued_frames | 22753 | 22323 | +1.89% |
| client.queued_wire_bytes | 4749336 | 4821768 | -1.53% |
| client.socket_read_bytes | 25657519 | 25537607 | +0.47% |
| client.socket_read_calls | 5924 | 5510 | +6.99% |
| client.socket_read_eagain | 5204 | 4781 | +8.13% |
| client.socket_write_bytes | 4749336 | 4821768 | -1.53% |
| client.socket_write_calls | 3943 | 3918 | +0.63% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 620 | 541 | +12.74% |
| host.command_batches | 4829 | 4401 | +8.86% |
| host.command_queue_hw | 35007 | 27566 | +21.26% |
| host.commands_executed | 65675 | 44645 | +32.02% |
| host.commands_submitted | 65676 | 44646 | +32.02% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 69797 | 57420 | +17.73% |
| host.events_queued | 87568 | 66969 | +23.52% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21892 | 22323 | -1.97% |
| host.response_ends | 21892 | 22323 | -1.97% |
| host.response_heads | 21892 | 22323 | -1.97% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1090 | 884 | 779 | 15954 | 8311 | 25662253 | 408776 |
| baseline | worker | 0.5277 | 2464 | 2369 | 1040 | 0 | 4749336 | 0 |
| candidate | gateway | 0.1023 | 876 | 777 | 14689 | 7477 | 25541913 | 358227 |
| candidate | worker | 0.5256 | 2564 | 2476 | 1030 | 0 | 4821768 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1603.4 | 8094 | 493 | 164 | - |
| baseline | worker | 7759.3 | 784 | 122 | 623 | - |
| candidate | gateway | 1534.8 | 7632 | 541 | 160 | - |
| candidate | worker | 7884.3 | 844 | 102 | 605 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
11.82%  [k] 0xffffffffb8864c81                                                                                                              
7.22%  [k] 0xffffffffb7e9212c                                                                                                               
1.75%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c
1.31%  [.] malloc                                                                                                                           
1.31%  [.] pthread_mutex_unlock                                                                                                             
1.31%  [.] sendmsg                                                                                                                          
1.09%  [.] capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)                                                               
1.09%  [k] 0xffffffffb7757919                                                                                                               

### baseline-worker (perf top, self)
23.27%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.58%  [.] lre_exec_backtrack                                                                                                               
4.99%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
4.59%  [.] malloc_usable_size                                                                                                               
4.20%  [.] malloc                                                                                                                           
3.81%  [.] cfree                                                                                                                            
2.14%  [.] find_own_property.lto_priv.0                                                                                                     
1.75%  [.] 0x00000000000ab758                                                                                                               

### candidate-host (perf top, self)
10.07%  [k] 0xffffffffb8864c81                                                                                                              
8.28%  [k] 0xffffffffb7e9212c                                                                                                               
2.01%  [k] 0xffffffffb884d8b5                                                                                                               
1.34%  [.] boost::asio::detail::reactive_socket_service_base::do_start_op(boost::asio::detail::reactive_socket_service_base::base_implementa
1.34%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::write_body_block(unsigned long, std::vector<unsigned char, std::allocato
1.34%  [.] malloc                                                                                                                           
1.12%  [.] boost::beast::basic_stream<boost::asio::ip::tcp, boost::asio::any_io_executor, boost::beast::unlimited_rate_policy>::ops::transfe
1.12%  [.] capsid::host::normalize_public_request(capsid::host::RequestRoutingPolicy const&, std::basic_string_view<char, std::char_traits<c

### candidate-worker (perf top, self)
24.12%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.48%  [.] lre_exec_backtrack                                                                                                               
4.46%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
4.42%  [.] malloc_usable_size                                                                                                               
3.95%  [.] malloc                                                                                                                           
3.65%  [.] cfree                                                                                                                            
2.70%  [.] find_own_property.lto_priv.0                                                                                                     
1.97%  [.] js_call_c_function.lto_priv.0                                                                                                    

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
