# A/B benchmark bodyless-11-ab2-20260802T174359

- commit: 15bd23a64efd6d777eb261890273aded28a3e1cf
- workload: fixed-1k, rounds: 3, warmup: 5s, measured: 10s
- connections: 1, inflight: 1, cpuset: none, tcp_nodelay: on
- baseline env: CAPSID_BODYLESS=0
- candidate env: CAPSID_BODYLESS=1
- evidence: complete

## Headline QPS (measured rounds)

| side | round | qps | p50_ms | p95_ms | p99_ms | dispatch_wait_ms | completed | errors | timeouts |
|------|-------|-----|--------|--------|--------|-----------------|-----------|--------|----------|
| baseline | 1 | 670.45 | 1.416 | 1.908 | 2.532 | 1.492 | 6706 | 0 | 0 |
| candidate | 1 | 678.08 | 1.404 | 1.849 | 2.522 | 1.475 | 6782 | 0 | 0 |
| candidate | 2 | 681.69 | 1.409 | 1.812 | 2.280 | 1.467 | 6819 | 0 | 0 |
| baseline | 2 | 624.09 | 1.503 | 2.118 | 2.908 | 1.603 | 6242 | 0 | 0 |
| baseline | 3 | 618.26 | 1.457 | 2.202 | 3.098 | 1.618 | 6184 | 0 | 0 |
| candidate | 3 | 679.23 | 1.404 | 1.855 | 2.487 | 1.473 | 6794 | 0 | 0 |

## Verdict

- frozen statistic: mean of the 3 measured rounds per side (frozen in the manifest before the run; the other statistic is reported only for context and never drives acceptance)
- baseline QPS: 637.60
- candidate QPS: 679.67
- delta QPS: +6.60%; delta p50: -3.62%
- host.commands_submitted/req: 3.0000 → 2.0000 (drop 33.33%)
- client.next_event_calls/req: 7.8989 → 6.0000 (drop 24.04%)
- client.parsed_frames/req: 4.0002 → 3.0002 (drop 25.00%)
- bodyless A/B (same binary, CAPSID_BODYLESS off→on): frozen per-request mechanism gate — all three metrics above must drop ≥20%, with no QPS regression
- verdict: PASS

### IPC mechanism counters (measured-rounds window)

| counter | baseline | candidate | drop % (positive = fewer) |
|---------|----------|-----------|----------------------------|
| client.flush_calls | 17731 | 14036 | +20.84% |
| client.next_event_calls | 35924 | 28074 | +21.85% |
| client.parsed_frames | 18193 | 14038 | +22.84% |
| client.parser_payload_copied_bytes | 4893719 | 5015959 | -2.50% |
| client.queue_would_block | 0 | 0 | +0.00% |
| client.queued_bytes_hw | 216 | 216 | +0.00% |
| client.queued_frames | 4548 | 4679 | -2.88% |
| client.queued_wire_bytes | 982368 | 1010664 | -2.88% |
| client.socket_read_bytes | 5330351 | 5352871 | -0.42% |
| client.socket_read_calls | 22280 | 18716 | +16.00% |
| client.socket_read_eagain | 17731 | 14036 | +20.84% |
| client.socket_write_bytes | 982368 | 1010664 | -2.88% |
| client.socket_write_calls | 4548 | 4679 | -2.88% |
| client.socket_write_eagain | 0 | 0 | +0.00% |
| host.asio_posts | 4583 | 4710 | -2.77% |
| host.command_batches | 13182 | 9358 | +29.01% |
| host.command_queue_hw | 2 | 1 | +50.00% |
| host.commands_executed | 13643 | 9357 | +31.42% |
| host.commands_submitted | 13644 | 9358 | +31.41% |
| host.credit_bytes_granted | 0 | 0 | +0.00% |
| host.credit_stall_count | 0 | 0 | +0.00% |
| host.event_queue_hw | 4 | 3 | +25.00% |
| host.events_queued | 18192 | 14037 | +22.84% |
| host.flush_calls | 0 | 0 | +0.00% |
| host.flush_eagain | 0 | 0 | +0.00% |
| host.grant_commands | 0 | 0 | +0.00% |
| host.response_body_frames | 4548 | 4679 | -2.88% |
| host.response_ends | 4548 | 4679 | -2.88% |
| host.response_heads | 4548 | 4679 | -2.88% |


## CPU/response and resources (profile runs)

| side | process | cpu_ms/response | rss_delta_kb | pss_delta_kb | read_syscalls | write_syscalls | read_bytes | write_bytes |
|------|---------|-----------------|--------------|--------------|---------------|----------------|------------|-------------|
| baseline | gateway | 0.8657 | 264 | 164 | 53190 | 36098 | 5343438 | 2945887 |
| baseline | worker | 0.7291 | 1256 | 1158 | 9096 | 0 | 982368 | 0 |
| candidate | gateway | 0.7910 | 336 | 218 | 42107 | 32908 | 5362134 | 3023393 |
| candidate | worker | 0.7067 | 1320 | 1225 | 9358 | 0 | 1010664 | 0 |

## perf-stat summary

| side | process | cpu_ms | context_switches | cpu_migrations | page_faults | unsupported |
|------|---------|--------|-----------------|----------------|------------|-------------|
| baseline | gateway | 2605.8 | 49105 | 3548 | 17 | - |
| baseline | worker | 2194.7 | 3649 | 714 | 286 | - |
| candidate | gateway | 2505.0 | 47477 | 4518 | 17 | - |
| candidate | worker | 2238.2 | 3713 | 822 | 287 | - |

## Dominant stacks (profile runs)


### baseline-gateway (perf top, self)
28.12%  [k] 0xffffffffb7e9212c                                                                                                              
16.58%  [k] 0xffffffffb8864c81                                                                                                              
2.85%  [k] 0xffffffffb8865585                                                                                                               
2.04%  [k] 0xffffffffb7757919                                                                                                               
1.36%  [.] write                                                                                                                            
1.36%  [k] 0xffffffffb884d8b5                                                                                                               
0.82%  [.] malloc                                                                                                                           
0.82%  [.] operator new(unsigned long)                                                                                                      

### baseline-worker (perf top, self)
14.20%  [.] JS_CallInternal.lto_priv.0                                                                                                      
6.64%  [.] lre_exec_backtrack                                                                                                               
5.09%  [.] malloc                                                                                                                           
4.78%  [k] 0xffffffffb8864c81                                                                                                               
4.17%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
3.09%  [.] malloc_usable_size                                                                                                               
2.78%  [.] cfree                                                                                                                            
2.31%  [.] add_property.lto_priv.0                                                                                                          

### candidate-host (perf top, self)
28.85%  [k] 0xffffffffb7e9212c                                                                                                              
14.71%  [k] 0xffffffffb8864c81                                                                                                              
3.96%  [k] 0xffffffffb8865585                                                                                                               
2.55%  [k] 0xffffffffb7757919                                                                                                               
1.56%  [k] 0xffffffffb884d8b5                                                                                                               
0.99%  [.] write                                                                                                                            
0.71%  [k] 0xffffffffb87d5f70                                                                                                               
0.57%  [.] capsid_worker_flush                                                                                                              

### candidate-worker (perf top, self)
21.88%  [.] JS_CallInternal.lto_priv.0                                                                                                      
6.16%  [.] malloc                                                                                                                           
5.86%  [.] lre_exec_backtrack                                                                                                               
4.01%  [.] malloc_usable_size                                                                                                               
3.70%  [k] 0xffffffffb8864c81                                                                                                               
3.39%  [.] JS_GetPropertyInternal.lto_priv.0                                                                                                
2.00%  [.] cfree                                                                                                                            
2.00%  [.] find_own_property.lto_priv.0                                                                                                     

Profiles: see baseline-gateway.pprof, baseline-worker.perf.data, candidate-host.perf.data, candidate-worker.perf.data, perf-stat/.
