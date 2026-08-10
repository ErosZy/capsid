# Capsid WP-08 完成报告（PR-13）— 安装、CPack 与 Release CI

- 执行日期：2026-08-09 ~ 2026-08-10
- 规格来源：`docs/capsid-remediation-execution-spec-2026-08-09.md` §12
- 分支：`wp01-05-correctness-chain`（PR-13 提交系列，含 WPT 门禁与发布前加固）

## 0. 范围与结论

WP-08 覆盖 spec §12 全部五项（§12.1 CPack 归属、§12.2 安装清单、§12.3 包格式
与可复现性、§12.4 package smoke、§12.5 CI 矩阵），另含 WPT 门禁（§12.5 Linux
Release 的 WPT 配置要求）与发布前加固提交。结论：

- Linux Release 全量 ctest（392 tests，含 84 个 WPT worker 门、3 个 package
  门、build identity matrix）：**392/392 通过**（`-j 2`，1387.67s）。
- TSan scope gates（388 tests，CI 同款排除）：结果见 §2（本轮数据）。
- ASAN scope gates（307 tests 配置，CI 同款排除）：结果见 §2（本轮数据）。
- 已知未在本机运行的门：macOS 平台门、UBSAN、GitHub hosted runner 上的
  strict sandbox（容器内 SKIP 属预期）、TSan 的 clang 变体（本机 GCC 已验证）。

## 1. 修改文件与关键函数

### 1.1 包与 CI（§12.1-12.5）

- `cmake/CapsidPackage.cmake`：CPack 归属（§12.1，阻止第三方接管）、SBOM
  与 build-info 打包、`CAPSID_SBOM_COMPAT_ID` 接线。
- `cmake/build_tests.cmake`：
  - WPT 区块（约 1975-2645 行）：manifest 锁定
    `1985b47aa8972a970f005957f2bfa036da1787c6`（rev-parse 精确匹配，
    不匹配 FATAL）；84 个 `worker_wpt_*` 测试（220-303）；batch1 四个
    legacy-mb HTML 文件（concat-sources.mjs）、extract-inline-script.mjs、
    idlharness（webidl2 规范化）、url-constructor（generate-resource-map.mjs）；
    `media/foo.vtt` 稀疏检出依赖（compression-stream.any.js 需要）。
  - worker=OFF 矩阵：install 按 target 存在性注册，package 门加 worker 门。
  - `worker_package_reproducibility`：TIMEOUT 1800、DEPENDS package_smoke。
- `.github/workflows/testing-validity.yml`：§12.5 矩阵 — Release Host ON +
  package evidence index（commit/build ID/compat ID/CTest JUnit/包 hash/SBOM
  hash）；asan/ubsan/tsan entries 各自 ctest_exclude；tsan entry 从 pinned
  源码（SHA-256 校验）构建 OpenSSL 3.5 到 `/opt/openssl35`。
- `cmake/ComputeBuildIdentity.cmake`：CAPSID_BUILD_* 镜像（WP-08 需要打包规则
  拿到与生成头一致的值）；`CMake regex 无 {n} 量词`（`[0-9a-f]+` + 长度校验，
  `if() MATCHES` 单参限制改 `string(CONCAT)`）。
- `tests/test_reproducibility.cmake`：空值 identity 记录比较（引号 +
  cmakeBuildType 空值正则）、cold-build 预算、glibc 特征宏、按路径分量泄漏
  扫描、allowlist 正则转义（`libstdc++.so` 编译错误）。

### 1.2 发布前加固（47c0ff3）

- `src/host/managed_host.cc`：active state 验证 fail-closed、整数边界
  （negative/zero/oversized workers 与 pool bounds 拒绝）。
- SSE MIME 大小写：`Text/Event-Stream` 等大小写变体与 `text/event-stream`
  参数化等价（`host_sse_uppercase_content_type_holds_permit`、
  `host_sse_parameterized_content_type_holds_permit`）。
- `src/host/*` poll timeout 饱和：统一 clamp 到 INT_MAX 防无限等待。
- AdminService/StaticPool start/stop 竞态闭环（stop gate + 原子回收）。

### 1.3 Worker 生命周期修复（PR-13 内，多为 WPT/框架门驱动）

- `src/worker_runtime.cc`：§7.4/§7.5 reclaim+teardown、timeout 路径先 poison
  再跑 timer continuation（288）、cancel 首轮 reclaim grace（e883803）、
  txiki 0015 patch（1018 CPU-timeout 崩溃）、promise reaction 创建时捕获
  job context（cc08e09，0014 patch）。
- `src/host/worker_executor.cc`：README/ready_match 握手一致性。
- `tests/test_host_managed_executable.cc`（fc60ba3）：`wait_for_socket` 从
  lstat 路径名探测改为 connect() 存活探测 — SIGKILL 崩溃注入阶段残留的 stale
  Admin socket 文件在 bind+listen 前会被探测为就绪（ECONNREFUSED 窗口）。
- `tests/test_worker_terminal_continuation.cc` / 框架 fixtures（843eb6c）：
  abort-aware delay/middleware-timeout fixtures。
- `tests/test_build_identity.cc`：`read_worker_identity` 要求 READY payload
  恰为 71 字节 sha256 ID。

### 1.4 WPT 覆盖审计（63c652a）

- CMP0057 修复（list 截断）；GB18030-2022 偏差闭环（legacy-mb 门）。

### 1.5 文档与许可

- `README.md` 同步到当前产品状态；新增 `LICENSE`（MIT）；
  `docs/txiki-upgrade-baseline.json`。

## 2. Gates 结果

| 门 | 命令 | 结果 | 用时 |
|---|---|---|---|
| WPT 门禁 | `ctest -R '^worker_wpt_' -j 2` | **84/84 通过** | 前次会话 |
| Linux Release 全量 | `ctest -j 2 --output-on-failure`（build-final，392 tests） | **392/392 通过** | 1387.67s |
| Package 门 | contents/smoke/reproducibility（全量内） | **3/3 通过** | — |
| 框架 5 门 | hono / itty-router×3 / h3-v2 lifecycle | **全通过** | — |
| Host 2 门 | host_managed_executable 家族（~16 tests）/ in-process host_managed 家族 | **全通过** | — |
| 身份门 | runtime_worker_compiler_identity_matches | **通过** | — |
| TSan scope | `ctest -j 2 -E '^(wpt_conformance_not_configured\|worker_strict_sandbox_direct_fetch\|worker_strict_sandbox_https_ca\|worker_package_.*)$'`（build-tsan，388 tests） | **388/388 通过**（最终复跑；首轮 385/388 的 3 失败分类见 §4） | ~200s |
| ASAN scope | 同款排除（build-asan，307 tests） | **301/301 通过**（最终复跑；首轮 296/301 的 5 失败分类见 §4） | ~4min |

## 3. 每条不变量

1. **WPT manifest 锁定**：/wpt 检出的 manifest 必须精确匹配锁定 commit
   `1985b47a…`，否则 configure 期 FATAL；门禁只对锁定内容成立。
2. **READY 承载身份**：worker 的 READY payload 恰为 71 字节 sha256
   compatibility ID（`static_assert` 编译期强制），host 与 bytecode-compile
   三者必须一致，不信任任一侧。
3. **identity 记录与二进制同源**：同一 configure 快照下，生成的
   build_identity.h、build-provenance-record.txt 与所有嵌入二进制的 ID 一致；
   跨 commit 复用旧 build 目录必须全量重建（fail-closed，见 §7）。
4. **managed 就绪 = 可 accept**：start_host() 的 READY 语义是 Admin listener
   可 accept 连接，不是 socket 路径名存在；崩溃注入残留的 stale socket 不得
   让后续 start 误报就绪。
5. **SIGKILL 不破坏后续启动**：kill_host 后的下一个 start_host 必须在新
   host bind+listen 之后才可连接（EADDRINUSE 证据规则 unlink+rebind 前，
   连接必须被拒）。
6. **包可复现**：同一 commit 同一配置两次 `make package` 的 tarball hash
   一致；package smoke 验证安装树内容、动态依赖 allowlist 与泄漏扫描。
7. **发布加固**：任何负值/零/超大整数配置在 deploy 期被拒（fail-closed）；
   active state 校验失败即拒绝发布；SSE MIME 大小写变体等价持有 permit。
8. **host 正常停止走 graceful**：shutdown→flush→有界等待→SIGTERM→SIGKILL
   升级链，任何一步分配失败都不得泄漏 child（§10.2 无分配 cleanup）。

## 4. RED 证据

- WPT 全灭（fixtures 缺失）→ 修依赖注册（fixture 是 test-worker-integration
  的依赖）；`/wpt/media/foo.vtt` 缺失 → 扩稀疏检出。
- test 64/65/68 stale-socket：`-j 8` 与 `-j 2` 双轮复现；strace 证明
  connect()（line 381768）先于 bind()（381773），fchmodat 已运行证明旧 host
  完成过 bind 序列；12 次对跑循环第 4 次复现，strace 下 5/23 失败。
- 修复后：12/12 对跑通过；单跑通过。
- TSan 首轮 82/388 失败 = build-tsan 跨 commit 陈旧（Aug 9 18:16 configure，
  身份头 00:52 重生成，部分二进制未重建）：test-build-identity 仍嵌旧 ID
  `fee71819…`，worker/库已嵌新 ID `21e1c645…`；全量增量重建后身份测试通过。
- TSan 复跑 385/388：#67 host_managed_http_e2e_multi_app（"managed Admin
  response timed out"）与 #177 host_single_worker_integration_metrics
  （`test_host_single_worker.mjs:408` /timeout status 503）solo 均通过
  （0.68s / 1.98s）→ TSan -j 2 负载 flake；#199 current_documentation_audit
  确定性失败（WP-08 报告为孤儿文档）→ docs/README.md 加导航链接修复。
- ASAN 首轮 296/301：#2 host_config_model + #58 enforces_queue_maximums 为
  **真实 bug** — `parse_duration_ms_text` 的
  `strtoull(text.substr(0,n).c_str(), &end)` 临时对象悬垂，`*end` 在完整
  表达式结束后读取 SSO 栈缓冲（stack-use-after-scope，ASAN 捕获；#58 是
  capsid-host 启动期同一崩溃）；修复 6d5fd23，solo 复跑通过。#174
  host_single_worker_integration（同 :408 /timeout 断言）与 #298
  worker_response_queue_saturation solo 通过 → 负载 flake。#289
  worker_build_identity_matrix 按设计 fail-closed：gate 运行时 worktree
  含未提交报告文件；报告提交后复跑。
- package_reproducibility 在 3 路并发（round-3 + 双 racer + rebuild）下
  1800s 超时；独占环境下通过（clean gate 内）。

## 5. GREEN 命令

```sh
# Linux Release 全量门（4 核容器，CI 同款 --parallel 2）
docker exec capsid-linux-bench sh -c 'cd /capsid/build-final && \
  ctest -j 2 --output-on-failure'          # 392/392, 1387.67s

# WPT 门（同一 build）
ctest -R "^worker_wpt_" -j 2               # 84/84

# TSan scope（capsid-tsan，GCC，CAPSID_ENABLE_TSAN=ON，Host ON，WPT 配置）
docker exec capsid-tsan sh -c 'cd /capsid/build-tsan && \
  ctest -j 2 -E "^(wpt_conformance_not_configured|worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca|worker_package_.*)$"'

# ASAN scope（capsid-linux-bench，Debug + ASAN，同款排除）
docker exec capsid-linux-bench sh -c 'cd /capsid/build-asan && \
  ctest -j 2 -E "^(wpt_conformance_not_configured|worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca|worker_package_.*)$"'
```

## 6. 未运行平台门

- macOS：本地无 runner；Host 单元 + non-strict worker 契约未跑（spec §12.5
  要求 SKIP 而非 FAIL 的负控项未验证）。
- UBSAN：spec §12.5 有矩阵 entry；本机未列为 WP-08 门（CI-owned）。
- strict sandbox（direct_fetch/https_ca）：容器无 seccomp delegated 环境，
  SKIP 属预期；须在 GitHub hosted runner 或 `--security-opt seccomp=unconfined`
  的 delegated 环境验证。
- TSan clang 变体：CI 矩阵用 clang（ubuntu），本机用 GCC 13（docs/testing.md
  记录：Alpine/musl clang 无 TSan 运行时；GCC 是本地支持编译器）。
- `wpt_conformance_not_configured`：本机 WPT 已配置，该排除项不存在（无害）。

## 7. ABI / 身份 / 持久化 / 包格式影响

- **ABI**：本 WP 无 `capsid_*` ABI 变更（v7 保持；WP-06 的 guard/枚举
  7/8 已在 PR-11 落地）。`capsid_event` 布局不变。
- **身份**：compatibility ID 语义不变，但跨 commit 复用 build 目录必须
  configure_file 重生成 + 全量增量重建；`release fail-closed`（干净
  worktree 检查）已在此前 WP-07 落地。本报告确认该设计在 sanitizer 构建
  下同样生效（§4 TSan RED 即此机制的负向证明）。
- **持久化**：generation/trusted key 存储布局不变（§9.3 激活事务在
  PR-10 已闭环）；本 WP 只加固读路径边界。
- **包格式**：tarball 内容由 §12.2 清单固定；SBOM/build-info 随包；
  复现 gate 两次构建 hash 一致；动态依赖 allowlist 只允许白名单 sonames。

## 8. 后续 WP 风险

- **WP-08 内已顺手关闭**：ASAN 捕获的 `parse_duration_ms_text` 悬垂 end
  指针（6d5fd23）——本类缺陷正是 sanitizer 门禁的 CI 矩阵价值。最终复跑
  TSan 388/388、ASAN 301/301，此前记录的两类负载 flake 均未复发。
- **WP-09 §13.1**：`src/client.cc` EOF/EXIT 构造（约 2204-2207 行）未清零
  flags/status/credit — 与 REQUEST_TIMEOUT 路径（2236-2246 已清零）不一致；
  复用 event 结构的上游会读到陈旧字段。
- **WP-09 §13.2**：hard timeout 只报告 map 中第一个超时 ID（`it->first`）；
  全部 inflight 请求需稳定 terminal reason。
- **WP-09 §13.3**：`capsid_worker_destroy` 语义需文档化为 abortive；
  Host 正常停止路径需显式 shutdown→flush→drain EXIT→destroy。
- **WP-09 §13.4**：`managed_host.cc` operation_registry 无界 static map
  （186-205 行）+ 每 app mutex 永久表（215-226 行）→ LRU/TTL + 生命周期绑定。
- **测试基建**：本机 4 核 + 双容器并发导致 package_reproducibility 与
  timing 类测试对负载敏感；CI 的 ubuntu runner（2 核起）需观察是否复现。
- **stale build 目录**：build-* 目录跨 commit 复用是 sanitizer 门的最主要
  假失败源；建议 CI 永远全新 configure（现状如此），本地脚本提示
  `--fresh`。
