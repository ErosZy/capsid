# M1D 跨机器交接快照

> 临时开发文档，记录时间：2026-08-03（Asia/Shanghai）。新机器恢复、核对并形成
> 可重放测试证据后删除本文件及文档导航链接。产品契约仍以
> [Host v1 详细设计](host-technical-design-review.md)和冻结测试为准。

## 1. 当前结论

- M0 已完成；M1A–M1C 的正确性、TSan 和性能证据已收尾。
- Bodyless fusion 的机制门通过，但独立性能门未通过；产品决定和未解决的饱和 p99
  风险记录在 [Bodyless 性能验收 waiver](bodyless-performance-waiver.md)。
- M1D 正在实现。compiler round-trip、artifact safe-read、secret provider、policy
  compiler 的基础实现和第一轮审计返修已经形成 commit。
- managed deploy/retire/recover 已保存为 `aeaca2c`；实施方报告六个真实 worker 场景通过，
  但尚待新机器独立审计和统一 sanitizer/平台门复核。
- Unix Admin API、完整恢复负控和 M1D 统一 sanitizer/平台/性能门尚未开始最终验收。
- 优先级保持不变：**先完成 managed 真实 worker 闭环，再接 Unix Admin API**。

任何路径在真实 worker 报告 READY、generation 已 durable commit 且 `active.json` 成功
发布之前，都不得返回 Active。`aeaca2c` 是可迁移的实现 checkpoint，在完成剩余审计门前
不能作为生产部署接口使用。

## 2. Git 保存点

记录本快照时：

```text
branch: main
managed checkpoint: aeaca2c feat(host): M1D managed pipeline — real worker closure, policy/secret wiring, active-state persist
CI follow-up:       e48f5b3 ci: install libboost-system-dev — capsid-host needs system Boost on runners
remote: origin/main 包含上述两个 commit 和本交接文档的最终修订
tree:   clean
```

已推送的五个基础返修 commit：

```text
b7a5336 fix(tools): publish safety — O_EXCL temps, no-replace publish, precise rollback, claim grammar
8e29bb4 fix(host): safe-read — three-file bytecode probe, per-file limits, ID grammar
f388d59 fix(host): secret provider contract — exact 16 KiB, key-id grammar, mtime recheck, single outcome
7870d6b fix(host): policy compiler semantics — no suffix wildcards, any-port reject, unlimited Host, canonical ordering
436ee4b build: round-trip test depends on capsid-bytecode-compile and capsid-worker
```

上述基础返修、managed 实现、冻结测试、CMake 接线和本交接文档已经一并包含在
`aeaca2c2f05fd0df4efea888c77f9e54156e4f7c`。Git object identity 已固定迁移内容；该 commit
是实现保存点，不代表 M1D 整体验收。

### 离开旧机器前

保存动作已经完成。离开前只需确认本地与远端指向同一 commit；不要再创建只存在于旧机器
的 stash：

```sh
git status --short --branch
git diff --check
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
git merge-base --is-ancestor e48f5b3098272ddf7d5b573377a43b7891e82553 HEAD
```

## 3. 新机器恢复

```sh
git clone --recurse-submodules git@github.com:ErosZy/capsid.git
cd capsid
git submodule sync --recursive
git submodule update --init --recursive
git checkout main
git pull --ff-only origin main
git status --short --branch
git log --oneline -12
git submodule status --recursive
```

恢复后必须看到上述五个基础返修 commit、managed checkpoint 和 CI follow-up：

```sh
git merge-base --is-ancestor e48f5b3098272ddf7d5b573377a43b7891e82553 HEAD
git status --short --branch
```

不要复制旧机器的 CMake build tree。新机器重新 configure，可以暴露路径、编译器、
OpenSSL 或未声明 target dependency 的问题。

## 4. 已完成的 M1D 基础切片

以下是实现保存点，不代表 M1D 整体完成：

| 切片 | 首个实现 commit | 审计返修 |
| --- | --- | --- |
| bytecode compiler CLI、canonical attestation、签名与确定性 | `39d1c7a` | `b7a5336` |
| compiler → attestation → 真实 worker round-trip 与篡改矩阵 | `08d18c8` | build dependency `436ee4b` |
| artifact safe-read、Linux `openat2`/dirfd walk、身份复核 | `3c54792` | `8e29bb4` |
| secret file provider、canary 零泄漏、revision | `82ec793` | `f388d59` |
| Host/App policy compiler、权限交集、effective metadata | `783922d` | `7870d6b` |
| managed coordinator 初始骨架 | `439c106` | 真实 worker checkpoint `aeaca2c` |

审计前四个冻结测试曾在 Linux Release 容器中独立通过；实施方报告 `aeaca2c` 新增的六个
managed 场景全部通过。上述结果仍需在新机器从干净 build tree 独立复核，普通、ASan、
UBSan、TSan 和跨平台统一门不能沿用旧 build tree 或口头结果。

## 5. Managed checkpoint 内容

`aeaca2c` 声明接入：

- 真实 attestation verification 和 trusted-bytecode/source fallback 选择；
- policy、secret 和 generation identity；
- 唯一 staging operation、文件同步、`COMPLETE` 和 generation commit；
- worker spawn/load/READY/compatibility 检查；
- 复用 active-state persist/parse API；
- retire/recover 和 App status 的严格状态读取。
- `host_managed` 冻结测试及 CMake 接线。

这些代码已保存并带有冻结测试，但尚未经过新机器的独立审计和统一 sanitizer 门；不能只
根据 commit message 把流程视为最终证明行为。该 commit 还更新了 workflow audit 固定的
artifact action v6 SHA，新机器应连同 CI 审计一起复核。

### 继续实现前必须复核的风险

1. 证明只有 `WorkerEventSource` adapter 接触 `capsid_worker_fd()`；不得扩大 CMake
   source-audit 豁免。
2. 构造 `capsid_capability_policy` 时，任何保存到 ABI struct 的 `c_str()` 指针都必须在
   spawn 调用期间稳定；vector 后续扩容不能让 rule resource 指针悬垂。rule ID 也必须唯一、
   稳定并来自 policy compiler，而不是统一填 `1`。
3. 不要在 managed 层维护第二套宽松 `capsid.json` parser。应复用冻结 schema、policy
   compiler 和 `compile_secret_snapshot()`；未知字段、重复字段、env 二选一及 secret 集合
   必须保持同一契约。
4. 证明 operation ID 生成并发安全；进程内普通静态计数器不能产生数据竞争。
5. state/staging I/O 应以预打开 dirfd 为边界，完整处理 EINTR、short read/write、文件类型、
   symlink 和 fsync 错误；不要退回绝对路径拼接或一次 `write()` 即认为完整。
6. `EEXIST` 只有在复核对象确实为本次期望目录/文件后才能接受。失败清理只能删除本次操作
   创建的对象。
7. 可信字节码选择必须验证签名、claims、source/bytecode digest、key、sourceName 和
   compatibility；compatibility fallback 只能按冻结规则发生。
8. generation identity 必须使用真实 config、effective policy、attestation、secret revision
   和 compatibility 输入，禁止零 digest 或空占位。
9. worker READY 后仍不能立刻宣告成功：generation durability 和 canonical `active.json`
   发布必须按设计顺序完成；发布失败时旧 active 不变，并销毁未接管的新 worker。
10. `retire` 必须写 canonical retired tombstone；`recover` 只信任严格解析的 active state 和
    `COMPLETE`，不得扫描并猜测 generation。

## 6. 后续一次性完成顺序

不要再把 M1D 拆成逐函数移交，按以下顺序完成一个合并批次：

1. 独立审计 `aeaca2c`，补齐上面的 managed 风险负控；
2. 覆盖源码、可信字节码、compatibility fallback、secret 进入 worker 四条路径；
3. 覆盖签名/claim/digest 错误、staging/fsync/READY 失败且旧 active 保持不变；
4. 完成 retire、restart recovery、stale temp、缺失/损坏 `COMPLETE` 负控；
5. 实现 Unix Admin API、peer credential 授权和四端点测试；
6. 执行 macOS native-dev、Linux Release、ASan、UBSan、TSan、真实 `openat2` 与完整回归；
7. 最后运行零探针性能回归，确认 M1D 不破坏 M1C baseline。

Admin API 必须最后接入，因为它只负责暴露已验证的 coordinator 语义，不能替未完成的部署
管线定义错误行为。

## 7. 新机器的首轮验证

先只验证基础切片和 build dependency：

```sh
cmake -S . -B build-m1d -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DCAPSID_BUILD_HOST=ON
cmake --build build-m1d --target \
  test-runtime-bytecode-compiler-round-trip \
  test-host-artifact-safe-read \
  test-host-secret-file-provider \
  test-host-policy-compiler
ctest --test-dir build-m1d --output-on-failure \
  -R '^(runtime_bytecode_compiler_round_trip|host_artifact_safe_read|host_secret_file_provider|host_policy_compiler)$'
```

随后构建全部 target。managed 集成测试变绿前，不要先跑长时间 benchmark，也不要开始对
QPS 作产品结论：

```sh
cmake --build build-m1d
ctest --test-dir build-m1d --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

Sanitizer、TSan 的环境要求和排除项见[测试与持续门禁](testing.md)。性能报告必须继续满足
[性能证据规则](performance-benchmarks.md)；`<not supported>` 硬件计数器在当前机器无 PMU
支持时可以记录为 unsupported，不伪造数据，也不作为功能失败。
