# Capsid 整改实施规格（供 DeepSeek 执行）

日期：2026-08-09  
设计基线：`b9fd07aa9886755e0cdbf4544b16a8932e93116c`  
配套审校报告：`docs/capsid-audit-handoff-2026-08-09.md`

## 1. 文档目的与执行纪律

本文只冻结整改方案、接口语义、工作包、测试门和提交边界。本次编写本文时不修改任何
产品源码。DeepSeek 后续实施时必须遵守：

1. 一次只执行一个工作包；每个工作包先加 RED 测试，再做最小实现，再运行规定矩阵。
2. 不直接修改 `vendor/txiki.js`；所有 vendor 改动必须进入 `patches/txiki` overlay。
3. 不通过删除断言、扩大超时、跳过平台或忽略事件让测试转绿。
4. 不把诊断探针当成正式回归；成熟后必须迁入 `tests/` 和 CTest。
5. 不在同一提交混入无关格式化、重命名或性能优化。
6. 每个提交必须记录：不变量、失败前证据、通过后证据、未覆盖平台和回滚方式。
7. 遇到本文未冻结且会改变公开 ABI、FetchRPC 或持久化格式的选择时停止实施，先补设计。

本文中的“必须”是合并门；“建议”允许 DeepSeek给出等价实现，但必须证明同一不变量。

## 2. 总体依赖顺序

```text
WP-00 冻结失败测试
  ├─ WP-01 BigInt 请求 ID
  └─ WP-02 RequestToken + QuickJS job context
       └─ WP-03 txiki 原生异步资源 + worker poison
            └─ WP-04 Host WorkerExecutor + replacement
                 └─ WP-05 Managed 数据面和激活事务

WP-06 C ABI 异常边界 ───────────────┐
WP-07 identity 拆分与唯一性 ────────┼─ WP-08 install/CPack/Release CI
WP-09 Runtime/P1 收尾 ───────────────┘
```

WP-01 至 WP-05 是一条正确性链，不能只合入中间一半后宣布请求隔离已修复。WP-06 和
WP-07 可在 WP-02/03 开发期间独立实施，但最终发行包必须在 WP-08 统一验收。

## 3. 全局设计决定

### 3.1 请求身份不是裸整数，而是不可复用 token

内部统一模型：

```text
RequestToken {
  request_id: uint64
  generation: uint64
  state: active | response_terminal | cancelled | timed_out | failed
  deadline_ns: uint64
  ref_count: bounded integer
}
```

- `request_id` 是传输身份；`generation` 区分同一 worker 内合法重用的相同 ID。
- token 状态只允许从 `active` 单向进入一种 terminal 状态，禁止复活。
- QuickJS job、txiki 原生 Promise、timer 和其他可恢复资源持有 token 引用。
- 当前执行上下文保存 `RequestToken*`，不再用单一 `executing_request_id_` 表达身份。
- native 能力入口必须同时验证：token 存在、状态为 active、显式 ID 与 token ID 相等。
- ID 0 只属于 worker-scope 控制事件；请求代码不能降级为 ID 0 后继续执行能力入口。

### 3.2 保守的 worker 复用策略

- cancel、timeout 或协议级 request failure 一律 poison 当前 worker。
- 正常响应结束后，若仍有 token-bound job 或原生资源，则 poison 当前 worker。
- poisoned worker 立即停止接收新 RequestHead；完成已有终态输出的有界 flush 后退出。
- Host 将 EXIT 视为容量下降并替换 worker；替换成功前不得把旧 worker 放回调度集合。
- 第一阶段不新增 FetchRPC frame：触发请求先得到既有 ERROR/timeout 或 response terminal，
  随后连接 EXIT。若实践证明必须区分 poison 原因，再单独设计 FetchRPC v4，不能偷用
  未登记 flag。

这会增加恶意取消导致的 worker churn，但它是隔离正确性优先的安全闭环。churn 由
Managed crash budget、startup permit 和退避控制，不能以“避免重启开销”为由继续复用
已污染 realm。

### 3.3 Managed 发布必须是“可失败准备 + 不可失败提交”

```text
stage/verify/commit generation
→ reserve weighted capacity
→ warm complete new pool
→ construct immutable routing snapshot         # may fail, disk untouched
→ persist + fsync active.json                  # may fail, route untouched
→ atomic publish routing snapshot (noexcept)   # must not allocate or fail
→ mark operation Active
→ drain/reap old generation asynchronously
```

retire 使用同一结构：先构造删除 App 的 snapshot，再持久化 retired tombstone，最后
`noexcept` 发布并 drain。禁止继续使用“先写 active.json，返回裸 worker，再由可能失败的
回调接管”的接口。

### 3.4 区分 bytecode compatibility 和 build provenance

- `compatibility_id` 只回答“此工具链能否安全读取该 QuickJS bytecode”。
- `build_id` 回答“这是哪一个 Capsid 源码、编译器、目标和构建配置产生的二进制”。
- trusted bytecode attestation 比较 `compatibility_id`。
- 发布物、READY 证据、SBOM 和 CI artifact index 使用 `build_id`，并同时记录
  `compatibility_id`。
- 在完成双 ID 之前，最小修复必须至少保证当前声明进入 compatibility record 的字段
  没有因 CMake list 截断而丢失。

## 4. WP-00：先冻结失败证据

### 4.1 目标

把 `build-m02-probe` 中已经证明问题的探针迁移成正式、最小、确定性的测试，但暂不改
实现。该提交预期在旧实现上失败，必须以 RED 提交或明确的预期失败分支保存，不能让主
分支永久带失败门。

### 4.2 新测试建议

1. `tests/test_worker_request_id_boundaries.cc`
   - 相邻 `2^53`、`2^53+1` 并发。
   - `2^64-1` 单请求和与 `2^64-2` 并发。
   - 每个 transport response、LOG 和 AUDIT 必须精确匹配。
2. `tests/test_worker_async_request_context.cc`
   - microtask、nested Promise、0ms/20ms timer、两个请求交错。
   - LOG、audit event、decoded audit record 三层逐项断言。
3. `tests/test_worker_terminal_continuation.cc`
   - cancel 后 timer、timeout 后 timer、正常 response 后 detached timer。
   - 验证能力入口被拒绝或 worker 退出，后续请求绝不能在旧 realm 执行。
4. 扩展 `tests/test_framework_worker_driver.cc`
   - 不再丢弃 LOG/AUDIT；按 request ID 收集。
   - Hono、itty-router、H3 fixture 各增加 await 前后和取消后续段。
5. `tests/test_build_identity_matrix.cmake`
   - 全新 normal/ASAN/UBSAN/mimalloc/LTO 配置生成记录并比较。
6. `tests/test_install_tree.cmake`、`tests/test_package_contents.cmake`
   - 空前缀安装不得为空；包名、文件清单和 smoke binary 必须正确。

### 4.3 禁止的测试替代

- 不接受仅测试 `src/protocol.cc` 的 uint64 往返；问题发生在 C++→JS 后。
- 不接受只检查框架自己的 AsyncLocalStorage；必须检查 Capsid native event。
- 不接受只 sleep 后断言进程存在；必须证明旧 realm 没有处理后续请求。
- 不接受复用旧 build directory 比较 identity；每个矩阵项必须全新 configure。

## 5. WP-01：BigInt 请求 ID 全链路

### 5.1 改动边界

- `src/worker_runtime.cc`
- `js/bootstrap.js`
- 上述 WP-00 request ID 测试
- 不改变 C ABI 的 `uint64_t`，不改变 FetchRPC wire format。

### 5.2 实现规则

1. C++→JS 的请求 ID 全部使用 `JS_NewBigUint64`：begin、body chunk、end、cancel 和
   `call_id_bridge()`。
2. JS→C++ 的请求 ID 全部使用 `JS_ToBigUint64`，替换请求相关的 `JS_ToIndex`。
   当前至少要逐项审计 `src/worker_runtime.cc:688,708,747,849,879,955,1024,1048`；
   不能机械替换用于长度、index 或容量的 `JS_ToIndex`。
3. `js/bootstrap.js` 的 `requests` 只使用 BigInt key；不得在日志字符串化之外调用
   `Number(id)`。
4. 所有显式 ID native bridge 在转换失败、ID 为 0、ID 不存在或 token 不匹配时抛出
  稳定内部错误，不得截断或取模。
5. 错误消息可用 `id.toString()`，但字符串不得反向成为状态 key。

### 5.3 验收

- 边界测试全部通过；现有低 ID 测试行为不变。
- 通过源码审计确认 request bridge 不再出现 `JS_NewInt64`/`JS_ToIndex`。
- `capsid:system` 中真正的数值容量仍保持 Number，不误改为 BigInt。
- ASAN/UBSAN 构建通过。

### 5.4 回滚

该工作包不改协议和持久化格式，可单独回滚；回滚后边界 RED 测试必须重新失败，证明
测试有检测力。

## 6. WP-02：RequestToken 与 QuickJS job 上下文

### 6.1 QuickJS overlay API

新增一个窄的 embedder job-context hook，不改变普通 QuickJS 用户的默认行为：

```text
capture(ctx, embedder_opaque, out_job_context) -> 0 | -1
enter(ctx, job_context, embedder_opaque) -> previous_context
leave(ctx, previous_context, embedder_opaque)
release(job_context, embedder_opaque)
```

建议公开为一个 `JS_SetJobContextHooks()` 结构体 API。确切 C 名称可调整，但语义必须是：

- `JS_EnqueueJob` 成功分配 job 后 capture 一次；返回 0 且 context 为 null 表示当前没有
  请求，返回 -1 才表示 capture 失败。失败必须使 enqueue 失败并释放已复制参数，不能
  创建身份不明的 job。
- `JS_ExecutePendingJob` 在调用 `job_func` 前 enter，所有正常/异常返回路径都 leave。
- job 正常执行后 release 一次。
- `JS_FreeRuntime` 清理未执行 job 时也 release 一次。
- 未安装 hook 时布局外行为与上游完全一致。
- hook 自身是 C callback，不允许异常越过 QuickJS C 边界。

overlay 建议新增 `patches/txiki/0012-capsid-async-context.patch`，不要直接改 vendor。
新增第 12 个 patch 时同步更新以下固定计数和审计：

- `cmake/ComputeTxikiOverlayKey.cmake`
- `cmake/AuditTxikiVendor.cmake`
- `tools/generate-txiki-upgrade-report.py`
- `docs/txiki-upgrade-baseline.json` 的 overlay key/manifest

### 6.2 WorkerRuntime token 所有权

- `WorkerRuntime` 增加单调 `next_token_generation_`、token registry 和
  `current_token_`。
- `ResponseState` 持有该请求 token 的 owner 引用。
- job hook capture 对 `current_token_` retain；enter/leave 支持嵌套并恢复前一 token。
- beginRequest bridge 必须在 token scope 内调用，使其创建的第一个 Promise reaction
  捕获 token。不能等到 JS job 内再调用 `capsidEnterRequest()`。
- `capsidEnterRequest`/`capsidLeaveRequest` 从 bootstrap 移除；若为了兼容暂留 native
  名称，只能作为断言/诊断，不能再成为身份权威。
- interrupt handler 从 `current_token_` 读取 ID/deadline/state，不再依赖裸全局 ID 再查
  `responses_`。

### 6.3 native 统一门禁

定义唯一内部函数，所有请求级 native API 先调用：

```text
require_active_request(ctx, optional_explicit_id)
  missing token                 -> throw InternalError
  terminal token                -> throw AbortError/InternalError
  explicit id != token.id       -> throw InternalError + poison worker
  registry generation mismatch  -> throw InternalError + poison worker
  otherwise                     -> return token
```

权限 query、stdio、fs、storage、fetch egress、request credit 和 response bridge 都必须
经过该函数。LOG/AUDIT 需要区分来源：应用模块加载阶段允许明确的 worker-scope ID 0；
请求执行期间必须取 active token；terminal token 不能再产生请求副作用。不得把“缺少
token”自动解释为 worker-scope，worker-scope 只能由显式 runtime phase 授权。
allow-audit 去重 key 不能只使用资源名；至少应包含 token generation，或明确将 allow
audit 设计为 worker-scope 且另外发出逐请求 operation audit。

### 6.4 JS bootstrap settled 信号

在 request Promise chain 的最终 cleanup 之后调用内部
`capsidRequestSettled(id)`：

1. `requests.delete(id)` 已完成。
2. C++ 标记 bootstrap chain settled。
3. 当前 job drain 完成后检查 token 引用。
4. 若只剩 registry owner，释放 token，worker 可复用。
5. 若仍有 job/native resource 引用，进入 poison 流程。

该 bridge 是唯一允许 terminal token 调用的内部生命周期入口：它必须验证 ID、token
generation 和 JS state 已删除，但不提供任何外部能力。不能在 `capsidResponseEnd()`
当场仅凭 refcount 判断，因为当前 reaction 和 `.finally()` 本身仍合法持有 token。

### 6.5 测试

- QuickJS 单元：enqueue/execute/exception/runtime-free 各自严格 retain/release 一次。
- 嵌套 job：A job 内执行 B callback 后恢复 A，再退出恢复 null。
- 两请求 job 交错：事件不串号。
- terminal job：JS cleanup 可运行，但任何 Capsid native 能力调用失败。
- hook capture 分配失败：enqueue fail closed，无泄漏。

## 7. WP-03：txiki 原生异步上下文与 poison

### 7.1 为什么还需要第二层

timer、HTTP client、stream 和 webcrypto 等 libuv callback 不是由 QuickJS job 直接
启动。它们若在注册时不保存 token，回调触发时 `current_token_` 为 null；之后才入队的
Promise reaction 仍会捕获错误上下文。因此 QuickJS job hook 不能替代 txiki resource
ownership。

### 7.2 txiki runtime hook

在 txiki overlay 增加与 WorkerRuntime 对接的 async-context hooks：capture、enter、
leave、retain/release。实现两个统一载体：

1. **`TJSPromise`**：`TJS_InitPromise` 在创建时 capture；`TJS_SettlePromise` 在调用
   resolve/reject 前 enter、之后 leave；`TJS_ClearPromise` release。所有失败路径必须
   exactly-once 清理。
2. **callback resource**：timer 等保存 JS function 的原生 struct 同时保存 captured
   context；libuv callback 用 contextual handler 调用，close/free 时 release。

优先覆盖当前受限 profile 实际可达的：timers、standard fetch/httpclient、streams、
webcrypto。被禁止的 process/signals/server/worker/websocket 仍应通过静态 inventory
标注为 unreachable；不能默认为“编译了但应用导入不到，所以永远不会间接调用”。

### 7.3 inventory 门

新增审计脚本枚举：

- 所有 `tjs_call_handler()` 调用点；
- 所有 `TJS_InitPromise`/`TJS_SettlePromise` 调用点；
- 所有保存 `JSValue callback/func` 并跨 libuv tick 使用的 struct。

每一项必须属于 `context-wired` 或 `profile-unreachable` 清单。出现新调用点而未分类时 CI
失败。此清单随 txiki upgrade report 一起归档。

### 7.4 poison 状态机

```text
Healthy
  ├─ cancel/timeout/failure ───────────────→ Poisoned
  └─ normal response + outstanding refs ──→ Poisoned

Poisoned
  → reject new RequestHead
  → terminalize all inflight requests once
  → bounded flush
  → close/exit
```

- poison 是幂等的，第一个原因获胜，后续只增加诊断计数。
- poison 后可以执行一个严格有界的 job drain，以便 Abort/stream cleanup 收尾；无需也
  无法可靠区分 cleanup job 和已经排队的应用 continuation。所有 terminal-token native
  能力入口均已拒绝，纯 JS 修改也不会跨到下一 worker。
- poison 使用独立的单调 `poison_deadline_ns`；terminal job 的有限/无限计算也受该期限
  interrupt。期限到达后直接退出，不等待用户 Promise。
- 正常 response 且无 outstanding resource 时继续复用，保持常规性能路径。

### 7.5 验收

- cancel/timeout 后能力入口不能产生外部副作用或 ID 0 事件。
- detached timer 不能影响下一请求；旧 worker EXIT，替换 worker 处理下一请求。
- direct fetch、webcrypto、stream callback 的请求 ID 正确。
- 1536 cancel memory test 继续平台化；新增 token retain/release 计数最终归零。
- 普通同步/async 请求无 detached resource 时不发生无理由重启。

## 8. WP-04：Host WorkerExecutor 与替换能力

### 8.1 必须先拆分的职责

不要给 `SingleWorkerServer` 简单增加一个“外部 worker 指针”选项。该类同时拥有 listener、
单 App route、session、admission 和 worker thread，无法支撑多 App Managed 路由。

从 `src/host/single_worker_server.cc` 提取内部 `WorkerExecutor`：

- 独占 `capsid_worker`、`WorkerEventSource`、command queue、event queue 和 worker thread。
- 接受已经 READY 的 adopted worker，或由 factory spawn/load/READY。
- 对外只暴露 submit/cancel/grant、event callback、health、inflight 和 stop/wait。
- `capsid_worker_destroy` 永远只在 owner/reaper thread 执行。
- `SingleWorkerServer` 改为组合 `WorkerExecutor`，先保证原有测试完全不变。

### 8.2 GenerationPool

```text
GenerationPool {
  app_id
  version
  generation_digest
  state: prepared | active | draining | dead
  vector<shared_ptr<WorkerExecutor>>
  immutable effective limits
  inflight counter
}
```

- 调度使用 least-loaded 或 Power-of-Two，负载至少包含 inflight、待客户端消费字节和
  unhealthy penalty。
- 请求开始时持有 `shared_ptr<GenerationPool>`；路由切换后已有请求仍固定在旧代。
- draining pool 不接收新请求；inflight 归零后交 reaper。
- drain deadline 到期时 cancel 剩余请求，因 WP-03 语义 worker 会退出并被回收。

### 8.3 replacement

- Worker EXIT 立即从 READY 集合移除。
- 同 generation、同 artifact、同 effective config 重新 spawn/load/READY。
- replacement 通过 per-App singleflight、全局 startup semaphore、指数退避和 crash
  budget。
- generation 已非 active 时禁止启动 replacement。
- replacement READY 前 pool 以 N-1 容量服务；降到 0 返回 503，不路由到 dead worker。

### 8.4 测试

- `SingleWorkerServer` 原测试作为无回归门。
- adopted READY worker 的所有权、启动失败和 exactly-once destroy。
- N→N-1→N replacement；retire 与 replacement race；Host shutdown 与 replacement
  race。
- poisoned EXIT 不误判为健康；新请求只进入 replacement。
- TSan 覆盖 command/event handoff、route publish、drain 和 reaper。

## 9. WP-05：Managed 数据面、可信键与激活事务

### 9.1 类型化 Host 配置

当前 `validate_config_json()` 后又由 `parse_managed_config()` 提取少数字段。整改后应由
一个类型化 `ParsedHostConfig` 表达所有已接受字段；schema 接受但实现忽略属于启动
错误，不能静默忽略。

至少包含：

- applications/state/secret roots、Admin socket；
- listeners 的 address/port/scheme/authority/routing/limits；
- trusted bytecode key descriptors；
- defaults、maximums、capacity、recovery；
- isolation required features。

无 listener 是合法 admin-only 模式；配置了 listener 就必须全部成功 bind，任一失败
使 Host 启动失败且不得发布 Admin readiness。

### 9.2 Listener 与 RoutingSnapshot

- 每个 listener 只拥有 socket、connection/header gate 和 immutable routing policy。
- 使用现有 `normalize_public_request()` 得到 App ID 和规范化 URL/header。
- `RoutingSnapshot` 是不可变 App→`shared_ptr<GenerationPool>` map。
- 请求在 route 时 atomic-load 一次 snapshot 并 pin pool；后续不得重新查 active route。
- path/subdomain/header 三种模式使用同一 normalization authority；header mode 只有明确
  trusted listener 才能启用。

### 9.3 激活事务 API

替换 `AsyncAdminBackendOptions.activate_worker/activate_pool` 裸指针回调。建议接口：

```text
prepare_activation(app, prepared_generation) -> ActivationPlan | error
persist_active(plan.active_document)          -> result
commit_activation(plan) noexcept
abort_activation(plan) noexcept

prepare_retire(app) -> RetirePlan | error
persist_retired(plan.retired_document) -> result
commit_retire(plan) noexcept
```

`ActivationPlan` 在 prepare 阶段已经拥有新 pool、完整新 snapshot 和旧 pool 引用；commit
只能 atomic-store，禁止分配、文件 I/O、锁等待或调用用户回调。persist 失败时 abort 新
pool，旧 route/active.json 均不变。commit 后操作才报告 Active，并异步 drain 旧 pool。

进程在 persist 成功、commit 前崩溃是允许的：重启以 active.json 为权威重新 warm 新代。
进程存活时 commit 不得失败，因此不会返回“操作 Failed 但磁盘已经指向新代”的状态。

### 9.4 weighted capacity ledger

当前 `WorkerCapacityPermit` 按 App 占一个 slot，无法表示 N-worker pool。v1 冻结新增
`capacity.activationSurgeWorkers`（非负整数，默认 0），并替换为计数账本：

- `workersTotal` 是 active generation 目标 pool 的 steady-state 总预算。
- `activationSurgeWorkers` 是新代 warming 与旧代 draining 重叠期间的额外预算；物理
  worker 绝对上限为两者之和，所有进程必须恰好记入一个类别。
- deploy 在任何 spawn 前一次性 reserve 目标 pool 数量；替换时超出 steady-state 的
  重叠部分占 surge。
- 没有 surge/headroom 时拒绝零停机 deploy，不能暗中超过绝对上限。
- old pool 只有在 reaper 完成后释放计数。
- startup concurrency 与 worker count 是两个独立限制。

### 9.5 可信公钥

- 配置 key path 必须绝对路径；以 `O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK` 打开。
- 文件必须是 regular、恰好 32 bytes raw Ed25519 public key、owner 为 root 或 Host euid、
  group/other 不可写；读取前后复核 dev/ino/size/mtime/ctime。
- key ID 设置长度和数量上限，重复 JSON key 已由 parser 拒绝。
- 使用拥有内存的 `TrustedKeyStore` 保存 `std::string` 和 32-byte array，再生成
  `TrustedBytecodeKey` string_view/span；禁止把 view 指向临时 vector。
- v1 key store 在进程启动后不可变；轮换通过重启加载新配置。恢复旧 generation 所需
  key 被删除时 fail closed，并给出不含路径/密钥的稳定错误。

### 9.6 Managed 端到端验收

1. 配置 listener 后 deploy，真实 HTTP 可达正确 App。
2. 新代 READY 但 persist 失败：旧代继续服务，新代销毁。
3. persist 后注入进程崩溃：重启恢复新代。
4. commit 后旧请求在旧代完成，新请求只进入新代。
5. drain deadline 到期：旧 worker 退出且容量归还。
6. retire 后新请求 404，旧 inflight 按 drain 契约完成/取消。
7. 多 App path/subdomain/header 路由不串 App。
8. trusted bytecode 正确 key 成功，unknown/short/swapped/changed key fail closed。
9. fixed pool N 的 capacity 计数是 N，不是 1。
10. Linux strict sandbox、ASAN/UBSAN/TSan 全部覆盖；macOS 只跑可移植 Host 单元。

## 10. WP-06：C ABI 异常封口和错误模型

### 10.1 结果码与错误详情

对 ABI v7 做加法扩展：

```text
CAPSID_OUT_OF_MEMORY = 7
CAPSID_INTERNAL_ERROR = 8
```

新增不分配内存的 thread-local 错误详情读取 API，返回的指针在同线程下一次 Capsid API
调用前有效。内部使用固定大小 char buffer；OOM 路径只写静态文本，不构造
`std::string`。

### 10.2 guard 规则

- 所有返回 `capsid_result` 的 extern C 入口经过统一 `abi_guard`。
- `std::bad_alloc`→`CAPSID_OUT_OF_MEMORY`；其他异常→`CAPSID_INTERNAL_ERROR`。
- 任何异常都不能越过 `extern "C"`，guard 自身必须 `noexcept`。
- `capsid_result_string()` 覆盖新增枚举和未知值。
- init/fd/pid 等不分配入口保持简单 `noexcept`。
- `capsid_worker_destroy()` 需要专用无分配 cleanup：即使 shutdown frame 分配失败也要
  close、TERM/KILL、waitpid 和 delete；不得仅 catch 后返回导致 child 泄漏。
- CPU topology 这类当前用整数返回错误的 API：内部异常时使用文档冻结的保守值并设置
  last error；长期可加返回 `capsid_result` 的新查询 API。

### 10.3 输入算术

在任何 reserve/insert/strlen 累加前做 checked-add/checked-multiply；descriptor count、
payload size 和 `size_t`→wire integer 转换统一验证。错误属于 INVALID_ARGUMENT 或
resource limit，不应依赖 allocator 抛异常才拒绝。

### 10.4 测试

- C 语言调用方编译链接测试，证明无 C++ exception 穿越。
- 可控 global `operator new` failure countdown 覆盖 spawn、policy copy、frame encode、
  request begin 和 response header decode。
- 每个失败点验证结果码、静态错误、fd/pid/child 数量和重复 destroy。
- ASAN/LSan/UBSAN；Linux 额外检查无 zombie。

## 11. WP-07：构建身份

### 11.1 先修 CMake list 截断

`CAPSID_BUILD_COMPILE_FLAGS` 必须一次构造为单一规范字符串，禁止多参数 `set()` 形成
semicolon list 后传给 one-value argument。`cmake_parse_arguments` 后若存在
`CGBI_UNPARSED_ARGUMENTS` 必须 configure fatal，防止以后再次静默截断。

### 11.2 compatibility record v2

只收录真实影响 bytecode 读取的字段，字段顺序固定、ASCII、每行 LF：

- QuickJS commit；
- txiki overlay manifest；
- QuickJS/bytecode compile definitions；
- target architecture、endianness、pointer width；
- bytecode format identity。

ASAN 等若经证明不影响 bytecode，可以不进入 compatibility；但不能继续处于“代码声明
进入、实际丢失”的状态。任何字段增删都提升 record schema，并更新 attestation
兼容策略。

### 11.3 build provenance record

新增 `build_id`，至少覆盖：

- Capsid git commit 和 Release clean-tree 标志；
- runtime/ABI/FetchRPC 版本；
- compatibility ID、capability manifest hash；
- compiler ID/version、target triple、CMake build type；
- LTO、ASAN、UBSAN、TSan、mimalloc、Host/worker feature flags；
- dependency lock/overlay key。

Release configure 在无法获得 commit 或 worktree dirty 时 fail closed；开发构建可生成
`dirty` provenance，但不得被 release packaging 接受。

### 11.4 暴露与测试

- `capsid_build_info` 以 struct-size 加法追加 `build_id` 和必要 provenance 字段，提升
  `CAPSID_BUILD_INFO_VERSION`。
- 同时修复真正的 size negotiation：新库必须接受旧 v1 `capsid_build_info` 的较小
  `struct_size`，只写调用方缓冲区容纳的完整字段；不能继续要求
  `struct_size >= sizeof(最新结构)`。增加“旧头文件编译、链接新库”的测试。
- worker READY 若暂不升级协议，仍携带 compatibility ID；Host 通过链接库 build_info
  和 worker/compile tool 的独立 CLI probe 记录 build ID。需要把 build ID 放入 wire 时
  单独升级 FetchRPC，不改变现有 71-byte READY 的含义。
- matrix 断言：相同源码/配置→相同 ID；每个受控 build 差异→不同 build ID；真正的
  bytecode ABI 差异→不同 compatibility ID。
- 测试必须能检测 record 字段缺失，不只验证“非空且 hash 自洽”。

## 12. WP-08：安装、CPack 与 Release CI

### 12.1 阻止第三方接管 CPack

在 txiki/libwebsockets overlay 中让 `include(CPack)` 和其 package metadata 只在
libwebsockets 作为顶层项目时生效。Capsid 顶层必须最后设置自己的 CPACK 变量并
`include(CPack)`；configure 后断言 `CPACK_PACKAGE_NAME` 为 `capsid`。

### 12.2 安装清单

最小 runtime 发行包必须包含：

```text
bin/capsid-worker
bin/capsid-bytecode-compile
bin/capsid-host                  # CAPSID_BUILD_HOST=ON
lib/libcapsid_runtime.*
include/capsid/runtime.h
include/capsid/runtime.hpp
lib/cmake/Capsid/CapsidTargets.cmake
share/doc/capsid/...             # 支持文档
share/licenses/capsid/LICENSE
share/capsid/build-info.txt
share/capsid/SBOM.spdx.json
```

不要安装测试 fixture、构建目录、私钥、secret snapshot 或 vendor 源码。worker 的运行时
依赖必须通过 RPATH/静态链接策略明确，不允许仅在 build tree 可运行。

### 12.3 包格式和可复现性

- 初始支持 TGZ；包名 `capsid-<version>-<system>-<arch>.tar.gz`。
- 固定权限、排序和时间戳，尊重 `SOURCE_DATE_EPOCH`。
- 生成 SHA-256、文件 manifest、build ID、compatibility ID 和 SPDX SBOM。
- 两次全新 Release 构建在相同输入下比较 manifest/hash；若编译产物尚不能位级复现，
  至少要求内容清单和 identity 一致并记录差异来源。

### 12.4 package smoke

CI 从压缩包解压到空目录，仅使用包内路径：

1. 编译一个 C 和一个 C++ public-header sample。
2. 运行 build-info probe。
3. 启动 worker、加载 bundle、完成请求和 shutdown。
4. 启动 Host、等待 readiness、发真实 HTTP 请求、SIGTERM graceful exit。
5. 扫描包中无 build-root 绝对路径、无 secret canary、无未声明动态依赖。

### 12.5 CI 矩阵

- Linux Release：显式 `CAPSID_BUILD_HOST=ON`、worker ON、LTO ON、WPT 配置、完整 ctest、
  delegated strict sandbox、package smoke。
- ASAN：Host ON + worker ON；只排除 sanitizer 本身不支持的 strict sandbox 用例，并在
  非 strict 路径覆盖完整数据面。
- UBSAN：Host ON + worker ON。
- TSan：Host ON + worker ON，覆盖 Managed route/replacement/drain；不得使用宽泛第一方
  suppression。
- macOS：Host 单元和非 strict worker 契约；平台不支持项必须 SKIP，不得 FAIL。
- hosted evidence index 同时记录 commit、build ID、compatibility ID、CTest JUnit、包
  hash 和 SBOM hash。

## 13. WP-09：P1 收尾包

该工作包在全部 P0 关闭后执行，不与 P0 混合提交：

1. EOF/EXIT 构造统一先清零 event 所有字段，再填 type/ID/payload。
2. 一个 worker 因 Host hard timeout 被杀时，为全部 inflight 请求生成稳定 terminal
   reason；不得只报告 map 中第一个 ID。
3. 明确 `capsid_worker_destroy` 是 abortive cleanup；graceful 路径必须
   shutdown→flush→drain EXIT→destroy。Host 所有正常停止走 graceful，超时才 terminate。
4. operation registry 使用有界 LRU/TTL；App mutex 随已发现 App 的固定生命周期拥有，
   不使用永久静态 map。
5. macOS artifact socket errno 和 Apple strip 参数按能力探测分支。
6. 加 24h/72h Managed soak：cancel/timeout、SSE、slow client、replacement、queue
   fairness、secret/key rotation 和 memory/token/refcount 平台化。

## 14. DeepSeek 提交拆分建议

### 14.1 预期文件触点

| 工作包 | 主要现有文件 | 建议新增文件 |
| --- | --- | --- |
| WP-00 | `cmake/build_tests.cmake`、framework driver/fixtures | request-id、async-context、terminal-continuation、identity/install/package tests |
| WP-01 | `src/worker_runtime.cc`、`js/bootstrap.js` | 无 |
| WP-02/03 | `patches/txiki/*`、overlay key/audit/report、`src/worker_runtime.cc`、baseline | `0012-capsid-async-context.patch`、async-context inventory audit |
| WP-04 | `src/host/single_worker_server.cc/.h`、`worker_event_source.*`、`cmake/build_host.cmake` | `worker_executor.*`、`generation_pool.*` 及单元测试 |
| WP-05 | `src/host/main.cc`、`managed_host.*`、`managed_admin_backend.*`、`config.*` | `host_config_model.*`、`managed_data_plane.*`、`trusted_key_store.*`、事务测试 |
| WP-06 | `include/capsid/runtime.h/.hpp`、`src/client.cc`、其他 C ABI translation units | ABI fault-injection/C-caller tests，必要时内部 `abi_guard.h` |
| WP-07 | `CMakeLists.txt`、`ComputeBuildIdentity.cmake`、`build_identity.h.in`、`build_info.cc`、compiler tool | identity matrix 和 provenance record template |
| WP-08 | 顶层/worker/host CMake、lws overlay patch、workflow | package config、CMake package config、SBOM/manifest/smoke scripts |
| WP-09 | Runtime/Host 对应实现与测试 | soak/fault-injection runner |

新增文件名是建议，不是接口契约；若 DeepSeek 选择不同命名，仍必须保持职责拆分，尤其
不能把 Managed router 再塞回 `main.cc` 或把 WorkerExecutor 留在单 worker benchmark 类
内部。

### 14.2 PR 顺序

建议一个 PR 只包含以下一个原子主题：

| PR | 内容 | 前置 | 主要门 |
| --- | --- | --- | --- |
| 01 | 正式 RED 测试与探针迁移 | 无 | 证明旧实现会失败 |
| 02 | BigInt 请求 ID | 01 | uint64 边界矩阵 |
| 03 | QuickJS job hook + RequestToken | 02 | job/token 单元 + async context |
| 04 | txiki native resource context | 03 | timer/fetch/crypto/stream |
| 05 | poison/exit 正确性 | 04 | terminal continuation |
| 06 | WorkerExecutor 提取 | 05 | SingleWorkerServer 全回归 + TSan |
| 07 | replacement 和 GenerationPool | 06 | N→N-1→N、race |
| 08 | typed config + key store | 可并行于 06 | config/key negative tests |
| 09 | Managed listener/routing | 07、08 | 多 App HTTP E2E |
| 10 | activation transaction + weighted capacity | 09 | crash/persist/drain |
| 11 | C ABI guard | 可并行 | fault injection + C caller |
| 12 | identity v2/build ID | 04、11 | fresh-config matrix |
| 13 | install/CPack | 10、12 | empty-prefix/package smoke |
| 14 | Release CI + P1 | 13 | 完整发布门 |

每个 PR 描述必须列出“未完成的后续 PR”，避免中间状态被误认为全部整改完成。

## 15. 交给 DeepSeek 的开工指令模板

```text
先阅读：
1. docs/capsid-audit-handoff-2026-08-09.md
2. docs/capsid-remediation-execution-spec-2026-08-09.md

当前只执行 WP-XX / PR-YY，不扩展到后续工作包。
先复核 HEAD 和工作树，保留所有用户改动。先提交或展示能在旧实现上失败的 RED 测试，
再做最小实现。vendor/txiki.js 不得直接修改，必须使用 patches/txiki overlay。

完成时报告：
- 修改文件与关键函数；
- 每条不变量如何实现；
- RED 失败证据与 GREEN 测试命令/结果；
- 未运行的平台门；
- 是否改变 ABI、FetchRPC、identity、持久化或包格式；
- 仍待后续工作包处理的风险。

禁止通过跳过测试、放宽断言、增加任意 sleep、关闭 strict warning/sanitizer 或吞掉未知
event 取得通过。如规格中的选择无法由当前源码确定，停止并给出两个方案及其兼容影响。
```

## 16. 最终完成定义

只有同时满足以下条件，DeepSeek 才能宣布整改完成：

- WP-01 至 WP-09 全部完成，七组 P0 均有旧失败/新通过的自动化证据。
- cancel/timeout/detached task 后旧 realm 不处理下一请求。
- Managed 配置的 listener 和 trusted keys 真正进入运行路径。
- active.json、routing 和 pool ownership 在故障注入下不分裂。
- C ABI fault injection 无异常逃逸、无 child/fd/memory 泄漏。
- compatibility ID 与 build ID 的语义和矩阵均通过。
- 空目录安装和最终 Capsid 包 smoke 通过，不再生成第三方 package。
- Linux Release Host+worker+WPT+strict sandbox 全绿；sanitizer 和 TSan 证据归档。
- 产品源码、测试、文档、SBOM、包 hash 和 CI evidence 指向同一提交与 build ID。
