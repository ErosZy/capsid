# M1P 第零门 evidence（e1b0414）

## 身份
- HEAD = origin/main = e1b0414ada81ef783dc51a8fc2cac598638dc823
- 工作树干净
- 源码 = f58545d（e1b0414 仅文档）

## Fresh build（build-e1b0414）
- gcc 13.3.0, -O3 -DNDEBUG, OpenSSL /opt/openssl35
- host SHA-256: 286684c031528b2fd447...
- worker SHA-256: c5a1e885bfd111f27112...（≠ 旧 3f7e8b2 0c8bf8f7）
- loadgen SHA-256: c76e386692d1f4395e21...

## 冻结测试（fresh build）
- queue-saturation 14/14 PASS
- outbound-buffer 5/5 PASS
- p0-boundaries PASS

## A/A（fixed-1k @ 64c，预热轮后 3 轮）
- qps: 1552 / 1543 / 1562（median 1552，离散 1.2% ≤5%）
- p99: 65.4 / 61.0 / 55.0（median 61，离散 9.8% ≤10%）

## 环境噪声
- 首轮冷启动偏低（1373 vs 稳定 1552）：预热轮治理后消除
- p99 尾部波动（55-65ms）：64 并发尾部固有，用 median 基准
- 磁盘 95%（宿主 VM 224G/234G）：单轮空间充足（build ~0.5G），每轮检查
- perf 不可用：容器 perf_event_paranoid=4 只读；用 worker 内置探针
  （CAPSID_PERF_DIAG）替代调用级归因

## 历史证据约束
- dual-ab-r2-20260804T143227：current 侧未绑定最新 commit + 无调用栈
  profile → 不作为本轮 baseline（仅历史诊断）
