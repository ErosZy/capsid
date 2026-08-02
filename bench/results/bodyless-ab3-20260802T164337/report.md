# A/B benchmark bodyless-ab3-20260802T164337

- commit: 4d8a7831dd9df10c6de7a7f4ee872dd8ec98bdf2
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 64, inflight: 64, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 1653.93 | 37.859 | 46.498 | 51.645 | 0.627 | 16549 | 0 | 0 |
| candidate | 1 | 1629.54 | 38.328 | 47.600 | 57.096 | 0.628 | 16343 | 0 | 0 |
| candidate | 2 | 1674.66 | 37.241 | 47.365 | 55.159 | 0.612 | 16759 | 0 | 0 |
| baseline | 2 | 1652.88 | 37.865 | 45.585 | 53.115 | 0.623 | 16578 | 0 | 0 |
| baseline | 3 | 1637.43 | 38.084 | 46.642 | 52.851 | 0.624 | 16404 | 0 | 0 |
| candidate | 3 | 1639.49 | 37.986 | 47.825 | 55.743 | 0.628 | 16448 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 1648.08
- candidate QPS: 1647.90
- delta QPS: -0.01%; delta p50: -0.22%
- host.commands_submitted/req: 4.4672 → 2.9876 (drop 33.12%)
- client.queued_frames/req: 1.5580 → 1.4938 (drop 4.12%)
- client.parsed_frames/req: 5.9563 → 4.4815 (drop 24.76%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: FAIL

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 4858 | 4560 | +6.13% |
| client.next_event_calls | 90087 | 70348 | +21.91% |
| client.parsed_frames | 85229 | 65788 | +22.81% |
| client.parser_payload_copied_bytes | 22926403 | 23507959 | -2.54% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 2783952 | 2900448 | -4.18% |
| client.queued_frames | 22294 | 21929 | +1.64% |
| client.queued_wire_bytes | 4626000 | 4736664 | -2.39% |
| client.socket_read_bytes | 24971899 | 25086871 | -0.46% |
| client.socket_read_calls | 5562 | 5277 | +5.12% |
| client.socket_read_eagain | 4858 | 4560 | +6.13% |
| client.socket_write_bytes | 4626000 | 4736664 | -2.39% |
| client.socket_write_calls | 3584 | 3728 | -4.02% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 592 | 552 | +6.76% |
| host.command_batches | 4488 | 4191 | +6.62% |
| host.command_queue_hw | 32916 | 28028 | +14.85% |
| host.commands_executed | 63920 | 43857 | +31.39% |
| host.commands_submitted | 63921 | 43858 | +31.39% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 70354 | 54509 | +22.52% |
| host.events_queued | 85228 | 65787 | +22.81% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 21307 | 21929 | -2.92% |
| host.response_ends | 21307 | 21929 | -2.92% |
| host.response_heads | 21307 | 21929 | -2.92% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.1078 | 840 | 741 | 14905 | 7819 | 24976292 | 390404 |
| baseline | worker | 0.5326 | 2592 | 2491 | 1005 | 0 | 4626000 | 0 |
| candidate | gateway | 0.1044 | 832 | 738 | 14024 | 7318 | 25090967 | 364671 |
| candidate | worker | 0.5248 | 2452 | 2354 | 1010 | 0 | 4736664 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 1541.9 | 7856 | 523 | 161 | - |
| baseline | worker | 7621.6 | 715 | 91 | 627 | - |
| candidate | gateway | 1532.4 | 7331 | 573 | 160 | - |
| candidate | worker | 7704.1 | 492 | 106 | 616 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
11.04%  [k] 0xffffffffb8864c81                                                                                                              
9.71%  [k] 0xffffffffb7e9212c                                                                                                               
2.43%  [k] 0xffffffffb7757919                                                                                                               
1.99%  [.] malloc                                                                                                                           
1.32%  [.] capsid_worker_next_event                                                                                                         
0.88%  [.] boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffer, boost::beast::htt
0.88%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)::{lambda(boost::system::e
0.88%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::write_body_block(unsigned long, std::vector<unsigned char, std::allocato

### baseline-worker (perf top, self)
23.05%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.26%  [.] lre_exec_backtrack                                                                                                               
4.78%  [.] malloc_usable_size                                                                                                               
4.74%  [.] malloc                                                                                                                           
4.26%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.56%  [.] cfree                                                                                                                            
1.76%  [.] find_own_property.lto_priv.0                                                                                                     
1.71%  [.] JS_FreeValue                                                                                                                     

### candidate-host (perf top, self)
10.16%  [k] 0xffffffffb8864c81                                                                                                              
7.67%  [k] 0xffffffffb7e9212c                                                                                                               
1.81%  [.] sendmsg                                                                                                                          
1.58%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::handle_worker_event(capsid::host::WorkerEvent)::{lambda(boost::system::e
1.35%  [k] 0xffffffffb7757919                                                                                                               
1.13%  [.] boost::beast::buffers_cat_view<boost::asio::const_buffer, boost::asio::const_buffer, boost::asio::const_buffer, boost::beast::htt
1.13%  [.] boost::beast::http::basic_parser<true>::put(boost::asio::const_buffer, boost::system::error_code&)                               
1.13%  [.] malloc                                                                                                                           

### candidate-worker (perf top, self)
22.28%  [.] JS_CallInternal.lto_priv.0                                                                                                      
9.76%  [.] lre_exec_backtrack                                                                                                               
4.93%  [.] malloc                                                                                                                           
4.80%  [.] malloc_usable_size                                                                                                               
4.23%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.57%  [.] cfree                                                                                                                            
2.05%  [.] find_own_property.lto_priv.0                                                                                                     
1.92%  [.] 0x00000000000ab758                                                                                                               

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
