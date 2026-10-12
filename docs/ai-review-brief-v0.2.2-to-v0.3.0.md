# Capsid 工程审计简报（供 AI 审校）

> 范围：v0.2.2 发布后的两个工程阶段——(A) 基准测量体系与 perf-site，
> (B) v0.3.0（FetchRPC v4 / WebSocket）的 TDD 实施。
> 基线 commit `aef6020`（tag v0.2.2）→ 当前 `51b65fe`（main，CI 全绿）。
> 本文档供独立审校：每节列出事实、证据位置、已知风险与待决问题。

## 0. 审校入口

- 仓库：`ErosZy/capsid`，主线 main；CI "Testing validity" 与
  "Performance site" 两个 workflow，最近一次（`51b65fe`）全绿。
- 关键文档索引：
  - 测量规范（normative）：`bench/SPEC.md`
  - v0.3.0 计划：`docs/v0.3.0-fetchrpc-v4-plan.md`
  - FetchRPC v4 契约（已冻结+修正案）：`docs/fetchrpc-v4.md`
  - workerd WS 语义档案：`docs/workerd-websocket-semantics.md`
  - 数据文档：`docs/performance-benchmarks.md`、`README.md` §Performance
  - 线上站点：https://eroszy.github.io/capsid/（GitHub Pages，Actions 部署）
- 提交序列（`aef6020..51b65fe`，共 29 个）：性能体系 21 个 +
  v0.3.0 计划与实现 8 个。逐条见 `git log --oneline aef6020..HEAD`。

## 1. 阶段 A：基准测量体系（已完成并上线）

### 1.1 三条车道（`bench/`，协议见 SPEC.md）

| 车道 | 脚本 | 关键协议点 |
| --- | --- | --- |
| 服务吞吐 | `qps-lane.sh` | 冻结 loadgen、64 连接/64 inflight、5s 预热+5×15s、逐轮字节精确校验、单 serving 进程/栈 |
| 冷启动 | `cold-start.sh`+`cold-start.cc` | **spawn→首个已验证响应**；total_ms 曾把 50 次 curl 热身循环计入（旧数据 ~400ms 平台是测量伪影，已修） |
| 密度 | `density-lane.sh` | 填充到 2048MB 预算、倍增+二分+抖动步退、冷(1 req/worker)/热(100 req/worker)双相位、触碰失败致命、双读稳定门 |

### 1.2 当前数据点（v0.2.2，全部生产姿态：managed 协调器 + Linux 强隔离）

| 指标 | capsid | workerd | FastAPI | Flask | PHP Slim |
| --- | --- | --- | --- | --- | --- |
| JSON-1k QPS | 9,490 | 26,624 | 12,948 | 3,568 | 2,095 |
| JSON-16k QPS | 7,109* | 20,189 | 12,599 | 2,878 | 2,008 |
| stream QPS | 6,523 | 14,192 | 6,671 | 3,987 | 2,032 |
| 服役内存 MB | 16.4 | 581.0 | 46.1 | 17.0 | 28.1 |
| 冷启动 10k/1m 字节码 ms | 17.7 / 31.1 | 20 / 38 | — | — | — |
| 密度 2GB 热 10k/1m | 882 / 132 workers | 638 / 214 cells | — | — | — |

\* 16k 跳变原因：旧车道钉死 `--initial-stream-window 1024`，managed
用生产默认——记录在 `five-qps-v022-managed-20261011/manifest.txt`。

**审校要点**：
- capsid 与其他栈**不同会话**测量（capsid=strict 重测，其他栈沿用
  `five-qps-v022-20261010`）——文档已声明两个证据目录，但严格的
  "same-session" 标准未满足，见 §1.4 R1。
- 隔离代价已量化：吞吐零损失、冷启动 +0.3~5.4ms、10k 密度 -1.6%。
- workerd 内存叙述已收紧为"采样窗口内未观察到回落"+ 等配置非等功
  限定（workerd 同窗口多服务 ~2.8× 请求）。

### 1.3 证据链规则（已固化进 SPEC + 收集器强制）

- 每点携带：git commit/tag、机器指纹、二进制/fixture/原始样本 SHA-256
- 收集器 fail-closed：重复 (stack,size) 格子拒绝；deno/node/workerd
  结果存在但指纹缺身份拒绝（曾拦下 "deno: absent" 与 deno 数据共存）
- 证据归档 `bench/results/<lane>-v<ver>-<date>/` + .gitignore 白名单
- 密度 manifest 显式记录 `CAPSID_STRICT=1` 与双相位

### 1.4 已知风险/待决（阶段 A）

| # | 问题 | 状态 |
| --- | --- | --- |
| R1 | capsid 严格沙箱重测与其他栈非同一会话 | 已声明；下次发布应单会话全栈重测 |
| R2 | capsid 共享 acceptor 的热身分布是连接哈希近似（已加倍请求量+注释） | 协议已言明；无 per-worker 计数验证 |
| R3 | 1m 密度受 QuickJS 急切编译限制（~1.4KB/函数元数据，实测 1MB 注释垫底=6.0MB vs 真代码=17.2MB） | 已调研成文；惰性编译列为 roadmap 级方向，非本版本 |
| R4 | celld 对比暂停（QPS/冷启动有数据：json-1k 48.9k、冷 ~9ms；密度未完成，因进程树层级/端口/就绪时序等测量问题连环） | 数据保留 /tmp；车道代码已回滚，未发布 |

## 2. 阶段 B：v0.3.0 FetchRPC v4 / WebSocket（进行中）

### 2.1 流程（严格 TDD，测试先于实现冻结）

已完成的工作流（对照 `docs/v0.3.0-fetchrpc-v4-plan.md`）：

| 工作流 | 交付物 | 测试 |
| --- | --- | --- |
| WS-A | workerd 语义档案（逐条引 6dd2348 的文件/行锚）+ 6 个带绝对断言的 fixture | runner 以 profile manifest 门控（exit 77，WebSocketPair 上线前跳过；上线后未接 harness 会大声失败） |
| WS-B | 契约 + 黄金向量（`vectors.json`） | 边界套件：现役 v3 解析器拒每个 v4 向量且错误串精确（"never mix silently" 在实现前锁死） |
| C1 | protocol v4：帧 19-26、版本化旗标、Hello 能力校验 | v4 往返/协商/坏能力向量全绿；v3 字节级不变 |
| C3a | RFC 6455 编解码（自含 SHA-1/base64） | RFC 向量 + 双向角色 + 长度形态 + 六类违规 |
| C3b | host 握手门禁（admission 前）：坏 key 400 / 版本 426+13 / 有效握手放行 | 真实 host 二进制 HTTP 驱动 |
| C-协商 | 端到端 v4：ABI 加法尾部字段 → client v4 Hello → worker negotiate 锁定 → READY 回告能力 → v4 流承载 v3 语义 | `test_worker_v4_negotiation.cc`：协商+载运+本地拒绝+v3 默认 |

### 2.2 契约修正案（审校重点：这些是实现对草案的"纠错"）

1. Hello v4 = **完整 v3 启动配置 + 8 字节能力尾**（草案的固定 87 字节
   忽略了配置载荷）——`fetchrpc-v4.md` §1.2 已记修正。
2. worker 能力经 **READY 尾部**回告（原草案无回复通道）。
3. 升级 RequestHead 由 host **预分配 channel_id**（id 权威留宿主）。
4. worker **引导期**接受首帧 3/4 并锁定（稳态矩阵不变）。
5. 帧号 19-26（草案 15-18 与 `kAudit`/`kMemoryMetrics*`/`kLoadBinding`
   冲突，审计发现后重编）。

### 2.3 ABI 兼容性设计（审校重点）

- `capsid_worker_config` 尾部加 `protocol_version/protocol_reserved/
  protocol_capabilities`（8 字节），`struct_size` 守卫读取。
- 惯例：`tests/compat/runtime_v7.h` 是 ABI 快照，加字段必须同步
  （本次 CI 抓到遗漏后已补——审校可检查快照与 runtime.h 逐字段一致）。
- 已知语义细节：`capsid_worker_config_init` memset `sizeof(*config)`
  （库视图），意味着"旧头文件编译 + 新库链接"的组合不被支持——
  与仓库既有惯例一致（源码兼容、非二进制兼容），但值得审校确认。

### 2.4 已知风险/待决（阶段 B）

| # | 问题 | 状态 |
| --- | --- | --- |
| B1 | 多段 python 替换脚本的断言失败会**静默回滚**前几段（negotiate 补丁曾因此丢失，靠运行期诊断发现） | 教训已记；建议审校核对所有"声称 applied"的补丁真实落盘（`git log -p` 可验） |
| B2 | worker stderr 被 `--close-stdio` 吞掉，调试只能走 /tmp 文件探针 | 临时手段已全部移除（`grep -r capsid-dbg` 应为空）；长期可考虑调试日志通道 |
| B3 | v4 协商测试的 v3-READY 分支只做了弱断言（不排除任意载荷尾 8 字节恰好为 (0,0)） | 测试文件内有注释说明；可在通道实现落地后改为强断言 |
| B4 | 密度车道曾有孤儿进程/端口复用/半孤儿车道互踩问题（celld 段暴露） | capsid/workerd 路径已修复（递归树遍历、父壳内联自增、杀全后代）；celld 回滚 |
| B5 | 通道注册表/双工泵/WebSocketPair/101 桥未实现（协商是"载运"阶段，worker 能力位=0） | 计划中的下一步；fixtures 仍 SKIP |

### 2.5 建议审校路径（按依赖序）

1. `bench/SPEC.md` ↔ 三条车道脚本 ↔ `bench/README.md` 的一致性
2. `docs/fetchrpc-v4.md`（含修正案标记）↔ `src/protocol.{h,cc}` ↔
   `tests/test_fetchrpc_v4.cc` + `vectors.json` 三方一致
3. `include/capsid/runtime.h` 尾部字段 ↔ `tests/compat/runtime_v7.h`
   快照逐字段一致；`src/client.cc` 的 struct_size 守卫读取逻辑
4. `src/worker_runtime.cc` 全部出向发射点（queue_output、两条
   response fast path、outbound_buffer）版本章完备性
5. 证据链抽样：任选 README 表格一个数字，沿
   `docs/performance-benchmarks.md` → 证据目录 manifest → cells.txt
   → sha256sums 追溯验证
6. CI：两个 workflow 的最近全绿；`workerd_ws_fixtures` 应为
   Skipped(77)、`fetchrpc_v4_boundary`/`ws_codec`/`worker_v4_negotiation`
   /`host_ws_upgrade_gates` 应为 Passed

## 3. 环境注意事项（审校者复现时）

- Release 构建有干净树门禁（`ComputeBuildIdentity.cmake`）；工作树有
  CRLF 噪音时需 `git stash push -u` → build → pop。本地迭代建议
  `build-dev`（RelWithDebInfo + `CAPSID_BUILD_HOST=ON`），首次配置需
  `npm ci --ignore-scripts --prefix examples/{hono,itty-router,h3-v2,elysia}-reference`。
- 推送凭据在 Windows 侧（PowerShell `git push`），WSL 内 PAT 权限不足
  （无 pages/workflow-dispatch 权限；rerun 也不可用，失败 job 只能用
  新提交重触发）。
- 密度/QPS 车道运行需安静环境；车道会独占 19400+/1899x 端口段并在
  失败时留下孤儿——SPEC 与脚本已含清理，但复现前建议
  `pgrep -af "celld|capsid-host|workerd"` 确认为空。
