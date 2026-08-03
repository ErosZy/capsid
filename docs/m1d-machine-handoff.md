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
- managed deploy/retire/recover 当前仍是未提交 WIP，尚未完成真实 worker 集成验收。
- Unix Admin API、完整恢复负控和 M1D 统一 sanitizer/平台/性能门尚未开始最终验收。
- 优先级保持不变：**先完成 managed 真实 worker 闭环，再接 Unix Admin API**。

任何路径在真实 worker 报告 READY、generation 已 durable commit 且 `active.json` 成功
发布之前，都不得返回 Active。当前 managed WIP 不能作为生产部署接口使用。

## 2. Git 保存点

记录本快照时：

```text
branch: main
HEAD:   436ee4b build: round-trip test depends on capsid-bytecode-compile and capsid-worker
remote: origin/main = 439c106（本地 main 领先 5 个 commit）
dirty:  src/host/managed_host.cc
        src/host/managed_host.h
        cmake/build_host.cmake
        cmake/build_tests.cmake
        tests/test_host_managed.cc（untracked）
```

尚未推送的五个基础返修 commit：

```text
b7a5336 fix(tools): publish safety — O_EXCL temps, no-replace publish, precise rollback, claim grammar
8e29bb4 fix(host): safe-read — three-file bytecode probe, per-file limits, ID grammar
f388d59 fix(host): secret provider contract — exact 16 KiB, key-id grammar, mtime recheck, single outcome
7870d6b fix(host): policy compiler semantics — no suffix wildcards, any-port reject, unlimited Host, canonical ordering
436ee4b build: round-trip test depends on capsid-bytecode-compile and capsid-worker
```

文档编辑期间另一个实现进程仍在继续写入 managed 集成；两次连续读取已经得到不同文件
摘要，因此这里不保存一个看似精确、实际已过期的 hash。离开旧机器前必须先停止并行
写入，再运行下方 `shasum` 命令并把最终输出保存到 WIP checkpoint 的 commit message、
外部迁移记录或 patch 同目录的 checksum 文件中。文件摘要只证明迁移完整性，不代表 WIP
已审计或可合并。

### 离开旧机器前

最安全的迁移方式是建立明确的 WIP checkpoint 并推送。不要依赖 `git stash`，因为普通
stash 不会随 `git push` 迁移到另一台机器。

```sh
git status --short --branch
git diff --check
shasum -a 256 \
  cmake/build_host.cmake cmake/build_tests.cmake \
  src/host/managed_host.cc src/host/managed_host.h \
  tests/test_host_managed.cc

# 审阅后把 managed WIP 与本交接文档作为明确的 WIP checkpoint 提交。
git add cmake/build_host.cmake cmake/build_tests.cmake \
  src/host/managed_host.cc src/host/managed_host.h \
  tests/test_host_managed.cc \
  docs/m1d-machine-handoff.md docs/README.md \
  docs/host-technical-design-review.md
git commit -m "wip(host): checkpoint M1D managed deployment handoff"
git push origin main
```

如果不允许提交 WIP，必须生成 binary patch 并通过仓库之外的可靠渠道复制；只保存在旧
机器 `/tmp` 中没有迁移价值：

```sh
git diff --binary -- \
  cmake/build_host.cmake cmake/build_tests.cmake \
  src/host/managed_host.cc src/host/managed_host.h \
  > capsid-m1d-managed-tracked.patch
git diff --no-index --binary /dev/null tests/test_host_managed.cc \
  > capsid-m1d-managed-test.patch || test $? -eq 1
tar -czf capsid-m1d-managed-wip.tar.gz \
  capsid-m1d-managed-tracked.patch capsid-m1d-managed-test.patch
shasum -a 256 capsid-m1d-managed-wip.tar.gz
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

恢复后必须看到上述五个基础返修 commit，以及迁移时创建的 WIP checkpoint。如果使用
外部 patch，则在干净 checkout 上执行并复核：

```sh
tar -xzf /path/to/capsid-m1d-managed-wip.tar.gz
git apply --check capsid-m1d-managed-tracked.patch
git apply capsid-m1d-managed-tracked.patch
git apply --check capsid-m1d-managed-test.patch
git apply capsid-m1d-managed-test.patch
git diff --check
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
| managed coordinator 初始骨架 | `439c106` | 当前未提交 WIP |

审计前四个冻结测试曾在 Linux Release 容器中独立通过；上述五个返修 commit 合并后的
普通、ASan、UBSan、TSan 和跨平台统一门仍需在新机器重新执行，不能沿用旧 build tree
或口头结果。

## 5. Managed WIP 已在尝试的内容

当前未提交差异正在接入：

- 真实 attestation verification 和 trusted-bytecode/source fallback 选择；
- policy、secret 和 generation identity；
- 唯一 staging operation、文件同步、`COMPLETE` 和 generation commit；
- worker spawn/load/READY/compatibility 检查；
- 复用 active-state persist/parse API；
- retire/recover 和 App status 的严格状态读取。
- `host_managed` 冻结测试及 CMake 接线。

这些代码尚未经过最终编译、冻结测试或 sanitizer 审计，不应把注释中列出的流程视为已
证明行为。

### 继续实现前必须审计的 WIP 风险

1. `managed_host.cc` 当前直接调用 `capsid_worker_fd()`；Host 契约要求只有
   `WorkerEventSource` adapter 可以接触该 POSIX fd。managed 层必须走 adapter，不能扩大
   CMake source-audit 豁免。
2. 构造 `capsid_capability_policy` 时，任何保存到 ABI struct 的 `c_str()` 指针都必须在
   spawn 调用期间稳定；vector 后续扩容不能让 rule resource 指针悬垂。rule ID 也必须唯一、
   稳定并来自 policy compiler，而不是统一填 `1`。
3. 不要在 managed 层维护第二套宽松 `capsid.json` parser。应复用冻结 schema、policy
   compiler 和 `compile_secret_snapshot()`；未知字段、重复字段、env 二选一及 secret 集合
   必须保持同一契约。
4. operation ID 生成必须并发安全；进程内普通静态计数器不能在多线程调用下产生数据竞争。
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

1. 修完上面的 managed WIP 风险并建立真实 worker 集成 RED；
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
