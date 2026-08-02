# A/B benchmark bodyless-11-ab1-20260802T174122

- commit: 15bd23a64efd6d777eb261890273aded28a3e1cf
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 1, inflight: 1, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 665.49 | 1.437 | 1.881 | 2.534 | 1.503 | 6656 | 0 | 0 |
| candidate | 1 | 679.51 | 1.406 | 1.866 | 2.426 | 1.472 | 6797 | 0 | 0 |
| candidate | 2 | 680.96 | 1.408 | 1.818 | 2.374 | 1.469 | 6811 | 0 | 0 |
| baseline | 2 | 665.45 | 1.432 | 1.909 | 2.578 | 1.503 | 6656 | 0 | 0 |
| baseline | 3 | 659.50 | 1.435 | 1.935 | 2.754 | 1.517 | 6596 | 0 | 0 |
| candidate | 3 | 672.83 | 1.403 | 1.952 | 2.565 | 1.486 | 6732 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 663.48
- candidate QPS: 677.77
- delta QPS: +2.15%; delta p50: -2.03%
- host.commands_submitted/req: 3.0000 → 2.0000 (drop 33.33%)
- client.next_event_calls/req: 7.8692 → 6.0004 (drop 23.75%)
- client.parsed_frames/req: 4.0002 → 3.0002 (drop 25.00%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 17066 | 13693 | +19.76% |
| client.next_event_calls | 34711 | 27386 | +21.10% |
| client.parsed_frames | 17645 | 13693 | +22.40% |
| client.parser_payload_copied_bytes | 4746307 | 4892679 | -3.08% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 216 | 216 | +0.00% |
| client.queued_frames | 4411 | 4564 | -3.47% |
| client.queued_wire_bytes | 952776 | 985824 | -3.47% |
| client.socket_read_bytes | 5169787 | 5221311 | -1.00% |
| client.socket_read_calls | 21478 | 18258 | +14.99% |
| client.socket_read_eagain | 17066 | 13693 | +19.76% |
| client.socket_write_bytes | 952776 | 985824 | -3.47% |
| client.socket_write_calls | 4411 | 4564 | -3.47% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 4460 | 4598 | -3.09% |
| host.command_batches | 12654 | 9128 | +27.86% |
| host.command_queue_hw | 2 | 1 | +50.00% |
| host.commands_executed | 13232 | 9127 | +31.02% |
| host.commands_submitted | 13233 | 9128 | +31.02% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 4 | 3 | +25.00% |
| host.events_queued | 17644 | 13692 | +22.40% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 4411 | 4564 | -3.47% |
| host.response_ends | 4411 | 4564 | -3.47% |
| host.response_heads | 4411 | 4564 | -3.47% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.8929 | 328 | 229 | 51195 | 34955 | 5182346 | 2866470 |
| baseline | worker | 0.7700 | 1196 | 1104 | 8822 | 0 | 952776 | 0 |
| candidate | gateway | 0.8303 | 336 | 238 | 41076 | 32118 | 5230344 | 2951447 |
| candidate | worker | 0.7544 | 1304 | 1207 | 9128 | 0 | 985824 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 2624.4 | 47181 | 3362 | 19 | - |
| baseline | worker | 2263.0 | 3514 | 697 | 284 | - |
| candidate | gateway | 2528.1 | 46665 | 4347 | 17 | - |
| candidate | worker | 2297.1 | 3612 | 749 | 288 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
27.13%  [k] 0xffffffffb7e9212c                                                                                                              
19.84%  [k] 0xffffffffb8864c81                                                                                                              
2.43%  [k] 0xffffffffb8865585                                                                                                               
1.89%  [k] 0xffffffffb7757919                                                                                                               
1.48%  [.] write                                                                                                                            
1.08%  [.] malloc                                                                                                                           
0.94%  [.] clock_gettime                                                                                                                    
0.94%  [k] 0xffffffffb78850ff                                                                                                               

### baseline-worker (perf top, self)
21.09%  [.] JS_CallInternal.lto_priv.0                                                                                                      
6.37%  [.] lre_exec_backtrack                                                                                                               
3.95%  [k] 0xffffffffb8864c81                                                                                                               
3.34%  [.] malloc_usable_size                                                                                                               
2.88%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
2.73%  [.] cfree                                                                                                                            
2.73%  [.] find_own_property.lto_priv.0                                                                                                     
2.73%  [.] malloc                                                                                                                           

### candidate-host (perf top, self)
34.88%  [k] 0xffffffffb7e9212c                                                                                                              
15.33%  [k] 0xffffffffb8864c81                                                                                                              
2.95%  [k] 0xffffffffb8865585                                                                                                               
2.67%  [k] 0xffffffffb7757919                                                                                                               
0.84%  [.] read                                                                                                                             
0.84%  [k] 0xffffffffb87d5f70                                                                                                               
0.70%  [.] boost::beast::http::detail::write_op<capsid::host::Impl::write_body_block(unsigned long, std::vector<unsigned char, std::allocato
0.70%  [.] write                                                                                                                            

### candidate-worker (perf top, self)
20.66%  [.] JS_CallInternal.lto_priv.0                                                                                                      
6.89%  [.] lre_exec_backtrack                                                                                                               
6.14%  [k] 0xffffffffb8864c81                                                                                                               
4.19%  [.] malloc                                                                                                                           
3.29%  [.] cfree                                                                                                                            
2.84%  [.] malloc_usable_size                                                                                                               
2.69%  [.] find_own_property.lto_priv.0                                                                                                     
2.54%  [.] add_property.lto_priv.0                                                                                                          

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
