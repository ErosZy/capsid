# Capsid 性能瓶颈归因与分阶段提升计划(2026-08-13)

**被测 commit**: `518995d`(含 perf/deep-serialization-ipc 合并)
**环境**: Lima `docker` VM(vz, x86_64, 4 核),容器与 formal run 同构
(capsid-bench/capsid:local, binary 身份 `d133e6ae`/`059dca6b` 与
four-stack-20260812T021308 完全一致)
**证据目录**: `bench/results/profile-20260813/`(4 个会话 × host + 4 worker
的 perf record --call-graph dwarf 原始数据 + 文本报告 + DIAG 探针 + host IPC
metrics + loadgen 样本)

profile 会话与 formal 数值一致性(环境无漂移):

| 会话 | 负载 | QPS | formal 对照 |
|---|---|---|---|
| S1 | json-1k c1 (4w) | 739, p50 1.23ms | four-stack 1.09ms |
| S2 | json-64k c64 | 1202 | 1199.8 ✓ |
| S3 | stream-64k c64 | 878 | 834.5 ✓ |
| S4 | json-1k c1 (1w, DIAG) | 同 S1,样本集中 | — |

## 1. 瓶颈归因(全部有 profile 支撑)

### 1.1 c64 饱和吞吐 —— worker 是瓶颈,JS 执行层主导

S2 (json-64k c64, 1202 QPS, svc 1.86 cores, 1.55ms CPU/请求) worker self:

| 热点 | self% | 归因 | 每请求 |
|---|---|---|---|
| JS_ToQuotedString | 38.5 | 应用 JSON.stringify 转义 64k 字符串,QuickJS 逐字节 | ~0.6ms |
| JS_CallInternal | 9.8 | QuickJS 解释器(children 76%,含上面 38.5) | 结构税 |
| JS_RunGC | 4.8 | 每请求 JS 对象分配压力 | ~74µs |
| lre_exec_backtrack | 3.7 | **两处 regex**: fetch.js 每 header 校验 + Hono RegExpRouter | ~57µs |
| malloc/usable_size/cfree | 5.7 | 每请求分配 churn | ~88µs |
| 属性机制(Get/Find/Add) | ~4.7 | shape 转换与属性定义 | ~73µs |

- host self 平坦: 最高是内核 `_raw_spin_unlock_irqrestore` 6.5%、`rep_movs` 3.5%
  (帧拷贝)、Beast 解析 ~1%;host 应用逻辑 (normalize/handle_worker_event) <2%。
  **c64 下 host 不是瓶颈**。
- Docker bridge 的 veth/conntrack/nf_hook 噪声 ~2-3%,对所有栈同等,不优化。

### 1.2 c1 固定开销 —— capsid 1.23ms vs ruby falcon 0.66ms

S4 (c1, 1 worker) DIAG 探针给出机制分解:

| 机制 | 测量 | 说明 |
|---|---|---|
| BRIDGE | 2 次/请求 × 95.7µs = **191µs** | JS↔native 桥调用 |
| DIAG handler | **378µs/请求** | 含桥;纯 JS 约 187µs |
| FLUSH | **3.00 write syscall/请求** | 每请求 3 个 worker 写 |
| host 侧 | 2 command batches + 1 asio::post + wake pipe 往返/请求 | IPC metrics 逐 pump 计数 |
| worker GC | 5.3% self | 与分配 churn 同源 |
| regex | 5.8% self | 同 1.1 |

p50 1.23ms 中 handler 只占 0.38ms,其余 ~850µs 是 **6 次跨线程/进程唤醒**
(io→executor wake pipe、executor→worker、worker→executor 读、executor→io
post、io→socket、client→io)在 c1 下无法摊薄的结构税。host profile 的
`_raw_spin_unlock_irqrestore` 28-36% self(唤醒管道的内核侧成本)是直接佐证。

### 1.3 流式路径 —— 公平比较下最大的差距

stream-64k c64: capsid 834 vs python(uvloop)1337。

- worker: JS_CallInternal 24.2%、promise 机制 (resolving functions/finalizer)
  ~1.7%、malloc 族 11%、GC 2.7% —— 16 个 4KiB chunk × promise 背压往返。
- host: Beast chunked serializer 迭代器链 (buffers_cat_view 族) ~3.2% self,
  `write_body_block` 每 chunk 一次 async_write + chunk framing + credit 往返。

### 1.4 已否决方向(避免重蹈)

- writev 批量发送(内容错乱,`eaebba4` revert)
- bootstrap promise chain 合并(QPS 崩到 127-163)
- 桥接 elision e4c903b / regex-free 扫描 3aa1f50 / 借 payload 8a0c61f —— 均被
  revert,原因未记录。**每个对应新实验必须先查 revert 根因再动手**。

## 2. 分阶段计划

每实验遵循 playbook TDD 闭环: profile 热点 → hypothesis.md → RED → 最小实现 →
正确性 + sanitizer → A/B → 保留或 revert。零探针正式证据用 lima-three-stack
formal;筛选轮用 ROUNDS=1。

### Phase 1:JS 侧每请求开销(低风险,预期 worker CPU −8~15%)

**E1 fetch.js header 校验 regex → 手写扫描器**
- 热点: lre_exec_backtrack 2.6-5.8%(fetch.js:44 名字校验、:54 值校验,每请求
  ~12-16 次执行)。历史上 3aa1f50 做过且被 revert —— **第一步是查 revert 根因**
  (分支内测试失败?证据不足?),若语义 bug 则修正后重做。
- RED: 冻结 normalizeIncomingInit 与 regex 实现在 header 语料库上逐字节等价
  (接受/拒绝集合全等,含 Unicode/边界字节)。
- 门: worker 自采样 CPU −3% 以上且零语义偏差,或 QPS +5%。

**E2 content-length 校验**(fetch.js:548 `/^[0-9]+$/`)同上处理。

**E3 header 名 toLowerCase 跳过**
- 热点: `js_string_toLowerCase` 0.46%。host normalize_public_request 已输出
  lowercase;JS 侧 trust 路径 (130dc41 已建立 brand) 不应再降一次。
- RED: 冻结 Request.headers 可见性测试(名字必须小写)与 130dc41 信任边界测试。

**E4 每请求分配削减(profile 引导)**
- 热点: GC 2.7-5.3% + malloc 族 5.7-11%。先加分配计数探针定位 bootstrap/fetch.js
  fast path 的临时对象(header 数组、URL 重建、Response 中间态),只动被计数
  证明的点。
- RED: 行为测试 + GC 次数/分配字节计数断言(可观测计数 RED)。

**E5 mimalloc A/B**(CMake 已有 `CAPSID_USE_MIMALLOC`)
- 一次 A/B 定去留: QPS +5% 或 allocator CPU 减 10%,否则弃。不写代码。

### Phase 2:桥接与 IPC 帧(中风险,预期 c1 p50 −10~15%)

**E6 bridge 成本拆解**
- 热点: 2 × 95.7µs = 191µs/请求。先拆 95.7µs 构成(native 分派 vs promise/job
  机制 vs 参数构造),只优化被拆解证明的部分。**不动 bootstrap 控制流**
  (M1P promise-chain 教训);候选: 参数构造复用、call_bridge 免 job hop 的
  隔离实验。e4c903b 的 elision 方向先查 revert 根因。

**E7 worker 写合并: 3 writes → 2**
- 先给 FLUSH 计数器加 per-frame-type 分解,定位第 3 个 write 是哪个帧
  (S4 计数 3.00 但帧构成未分解)。若为可推迟的 credit/window 帧,实验有界聚合。
- fixed+end 单帧融合需要 wire protocol 终局标志 → **不在循环内**,列入 §3
  架构清单单独审计。

**E8 host 侧帧拷贝消除**
- 热点: `rep_movs_alternative` 3.5% (S2) = parser payload copy +
  `WorkerEvent.body.assign` 对 64k 响应的拷贝。8a0c61f borrow 被 revert ——
  先查根因;安全变体用 **move**(parser buffer chunk 转移所有权)而非 borrow。

### Phase 3:Host 唤醒与调度(c1 与 host CPU)

**E9 wake pipe 有界聚合**
- 热点: host `_raw_spin_unlock_irqrestore` 28-36% (c1)、每请求 2 command
  batches。c64 下 host CPU 占比低,此项主要服务 c1 与 CPU/请求指标。
- 约束: shutdown/timeout/cancel 的最大唤醒延迟冻结为 RED 测试。

**E10 P3 owner-loop 原型(feature-flag)**
- 只有 E1-E9 收敛后、且 profile 仍显示跨线程 handoff 显著时才启动。原型必须
  证明 ≥10% QPS 或 15% p99,否则撤销(playbook P3 门槛)。

### Phase 4:流式路径(最大公平差距)

**E11 credit 聚合阈值扫描**
- `CAPSID_CREDIT_GRANT_THRESHOLD` 已存在(默认 0=立即)。用 tune 模式扫
  {0, 8KiB, 16KiB, 32KiB} × stream-64k,零代码成本,先拿数据。

**E12 host 写合并**
- `body_queue` 已按序排队,但每 block 一次 async_write。实验: 完成回调里把
  已缓冲的多 block 合并为一次 `http::async_write`(buffer sequence),
  partial-write 状态机必须可恢复。
- RED: 慢客户端 + 并发 partial write 的冻结测试(M1P writev 事故的教训)。

**E13 流式 promise 机制微优化**
- worker `js_create_resolving_functions`/`js_promise_finalizer` 路径,目标是把
  每 chunk 的 promise 往返降为可观测更少的 job hop。风险高,放最后,门槛同 E6。

### Phase 5:战略项(不在优化循环内,另行决策)

- **QuickJS vendor 优化**: JS_ToQuotedString 38.5%(json-64k)逐字节实现;
  评估 SIMD 化转义或 QuickJS JIT fork(M1P §6 已定性为独立评估项目)。
- **fixture 公平性**: capsid 侧 Hono RegExpRouter 每请求跑大 regex 而
  Flask/Slim/Falcon 是 trie/线性路由;ruby 侧预计算 body。要么 capsid 换
  Hono LinearRouter,要么四栈统一语义 —— 这是基准协议问题,不是 runtime 优化。
- **wire protocol 终局**: fixed 帧带 terminal 标志、window/credit 帧合并 ——
  playbook §8 硬停止项,需单独设计审计。
- **进程拓扑**: Go gateway in-process (cgo) 模型在 fixed-1k 领先 C++ host
  ~20-25%(M1B 首轮),进程边界的税是 c1 架构税的主体;与 owner-loop 一起
  属于架构级评估。

## 3. 预期总收益与验收

| 指标 | 现状 | Phase 1-4 后目标 | 依据 |
|---|---|---|---|
| json-1k c64 QPS | 2998 | +8~15% | E1-E5 削减 worker CPU |
| json-64k c64 QPS | 1200 | +5~10% | JSON.stringify 主导,空间有限 |
| stream-64k c64 QPS | 834 | +20~40% | E11-E13 攻击 credit/write 往返 |
| json-1k c1 p50 | 1.09-1.23ms | 0.9-1.0ms | E6-E9;低于此需 E10/架构级 |

## 4. 执行记分板(2026-08-13 循环,截至 313bc47)

| 实验 | 机制 | 结果 | 证据 |
|---|---|---|---|
| E11 credit 阈值默认 16KiB + window/4 clamp | 流式响应 credit 批量化 | **ACCEPTED** stream-64k +3.0%(4 轮交错,0 错误) | `5b285c8` |
| E4a bridgeHeaderPairs 单遍提取 | 去掉 sort/iterator/forEach 闭包/slice 分配 | **ACCEPTED** json-1k **+5.4%**、p99 −6.7%(3 轮) | `93e23cf` |
| E4b 入站 init 就地归一化 | 去掉 spread/entries/pair 数组分配 | **ACCEPTED** json-64k +3.0%、stream-64k +1.9% | `92df467` |
| E13a 非阻塞流写跳过 promise 机制 | 每 64k 流式响应少 16 个 promise+resolve+await | **ACCEPTED** stream-64k +3.3%(3 轮) | `313bc47` |
| E1 header 校验 regex → 手写扫描 | — | **REVERTED** json-1k **−3.0%**(5 轮):短字符串上 JS 逐字符循环慢于 QuickJS 原生 regex | `f2abf71`→`0997a4f` |
| E5 mimalloc | txiki 自带 mimalloc 开关 | **FAIL** 启动即 std::bad_alloc(宿主下必现,独立跑不复现);vendored mimalloc 过旧,根因排查超出本循环预算 | 未提交 |
| E14 stream window 64→128KiB | 窗口扫描 | **无效果**(首轮 +34% 是并发构建抢核伪影,干净 A/B 中性) | 未提交 |
| E7 帧合并(3 writes→2) | — | **不可做**:M1P writev 已失败过;END 融合需 wire 协议变更(playbook 硬停止) | 仅探针 `c6b9917`(head/body/end 各 1 帧/请求) |
| Hono LinearRouter | fixture 路由 | **更慢** −8%(QuickJS 原生 regex 快于 JS 循环),RegExpRouter 已是 QuickJS 下的最优 fixture | 未提交 |

**累计**(worker 基线 `059dca6b` → 最终二进制,零探针 screening,交错轮):json-1k c64 **+6.6%**(3235→3450)、json-1k c1 **+3.0%** 且 p50 1.22→1.18ms、json-64k +3.0%、stream-64k +3.3%、stream-16k +2.5%,0 错误。E4a 单独达 QPS +5% 门槛;其余为机制改善(wavier 类)。E11(host 侧)+3.0% stream-64k 另计(env 等价证据)。

**门禁**:Release RED 全套通过(worker integration 全模式、queue-saturation #1-5、host-single-worker mjs + lifecycle、credit-limits 单测);ASan+UBSan(含 detect_leaks)下 worker integration 全模式 + queue-saturation 通过。TSan 与 lima-three-stack 正式 zero-probe 轮次留给用户侧。

**收敛判定**:当前 profile 中剩余热点为 ①JS_CallInternal ~18.5% + JS_ToQuotedString(QuickJS 解释器,vendor 禁改)②lre_exec_backtrack ~7%(已证明是 QuickJS 下最优 fixture)③属性/GC/分配机制 ~12%(剩余削减空间 ≤1-2%/项)④内核唤醒与网络栈(结构级)。循环内可安全消除的成本已收敛;下一档需要 wire 协议融合或 owner-loop 原型(均超出本循环,见 §3 架构清单)。

**未完成门禁**:TSan 与 lima-three-stack 正式 zero-probe 轮次留给用户侧;screening 已按同协议 cpuset/交错/多负载执行。

## 5. 执行纪律(继承 playbook)

- 每实验独立 commit + evidence commit;revert 用新 commit。
- 零探针正式证据用 lima-three-stack formal;DIAG/IPC metrics 只在诊断轮开。
- 硬停止条件沿用 playbook §8;连续 3 个 fail 后回到本文 §1 重新 profile。
