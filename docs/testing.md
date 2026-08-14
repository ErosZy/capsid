# 测试与持续门禁

项目把自有契约、适配 WPT、进程集成和环境型沙箱验证分开报告。任何一层通过
都不能替代另一层。

## 测试分层

1. C/C++ 单元测试：协议、header、策略、拓扑、审计与结构化解析；
2. 真实 worker contract：bundle、IPC、流控、取消、超时、sandbox 和 fetch；
3. 固定 WPT：manifest 选择的每个上游文件都在独立 worker realm 中执行；
4. 框架差分：Node reference 与真实 worker 逐向量比较，并保留独立绝对断言；
5. sanitizer/fuzz：项目 target 的 ASan、UBSan、计划中的 Host TSan 和四个
   libFuzzer harness；
6. benchmark contract：先验证内容、版本和环境，再允许记录性能样本。

标准来源、WPT 选择和偏差统一见[标准与合规](conformance.md)。

## 有效性规则

长期门禁采用 fail-closed：

- 未配置 WPT 时登记 `wpt_conformance_not_configured` 失败哨兵；
- checkout 必须等于 manifest 固定 commit；
- manifest 与实际注册路径逐项比对，不只比较数量；
- WPT realm 必须报告顶层唯一、正整数 `total` 和
  `ranNothing: false`；
- expected failure 与 `notExecuted` 只从 manifest 读取，并必须引用已登记偏差；
- restricted 二进制审计必须先证明符号表、archive 和 LTO marker 可读；
- Wasm 的 C、JavaScript 和 fixture 资源常量必须一致；
- overlay key、stamp、manifest 和 configure dependencies 必须来自同一
  canonical 输入；
- 三个框架的差分向量必须有独立绝对断言，anchor 数不能为零；
- 负控必须证明门禁能捕获错误输入，不能只有“正确实现会绿”的正向测试。

测试有效性由上述自动化门禁持续证明；一次性审计报告会与当前代码漂移，因此不作为
产品状态文档保留。

## 常用命令

基础构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

固定 WPT：

```sh
cmake -S . -B build-wpt \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_WPT_ROOT=/absolute/path/to/pinned/wpt
cmake --build build-wpt
ctest --test-dir build-wpt --output-on-failure
```

WPT 根目录的 `HEAD` 必须等于 `tests/wpt/manifest.json` 中的 commit。

### Egress 与 TLS 定向回归

域名 egress 规则授权该域名实际解析出的地址，包括 private、loopback、
link-local 和其他 protected range；显式 IP/CIDR deny 仍优先。该授权只属于以
域名发起的请求，不会把同一地址变成可直接请求的 IP literal。策略单测和真实
worker 回归分别由 `egress_policy`、
`worker_fetch_hostname_authorizes_resolved_loopback`、
`worker_fetch_host_deny_diagnostic`、
`worker_fetch_protected_deny_diagnostic`、
`worker_fetch_explicit_deny_diagnostic` 与
`worker_direct_fetch_http_matrix` 覆盖。最终授权语义和三类诊断文本见
[宿主能力策略](capability-policy.md)。

固定的 mbedTLS 版本曾在混合 TLS 1.2/1.3 客户端配置下出现签名算法检查不一致：
`mbedtls_ssl_get_pk_type_and_md_alg_from_sig_alg()` 能解析 TLS signature scheme
`0x0804`（`rsa_pss_rsae_sha256`），但 TLS 1.2 的旧式 hash/signature byte-pair
检查把高字节 `0x08` 当作未知 hash 并拒绝 ServerKeyExchange。客户端已经在
ClientHello 中声明该 scheme，TLS 1.2 服务端选择它也是合法行为，因此这不是服务端
或应用配置问题。

`patches/txiki/0009-lws-vendor.patch` 只在 TLS 1.2 客户端解析
ServerKeyExchange 的失败分支兼容 `rsa_pss_rsae_sha256/384/512`：每个分支仍受
对应 hash 与 PKCS#1 v2.1 编译能力约束，后续“服务端选择的 scheme 必须由客户端
实际 offered”检查保持不变。修复没有放宽通用 TLS 1.2 helper，也没有通过强制
TLS 1.2-only 来规避协商。

`worker_direct_fetch_https_tls12_rsa_pss` 启动本地 OpenSSL `s_server`，强制
`-tls1_2 -sigalgs rsa_pss_rsae_sha256`，然后让真实 worker 完成受信任 HTTPS
请求；测试带 `--strict`，同时要求请求成功和 worker 干净退出。定向复现命令：

```sh
ctest --test-dir build --output-on-failure \
  -R '^(egress_policy|worker_fetch_.*diagnostic|worker_fetch_hostname_authorizes_resolved_loopback|worker_direct_fetch_http_matrix|worker_direct_fetch_https_tls12_rsa_pss)$'
```

ASan 示例：

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCAPSID_ENABLE_ASAN=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure \
  -E '^(worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca)$'
```

UBSan 将开关替换为 `CAPSID_ENABLE_UBSAN=ON`。fuzz 构建使用 Clang、
`CAPSID_BUILD_WORKER=OFF` 和 `CAPSID_BUILD_FUZZERS=ON`。

### TSan（M1C 门）

`CAPSID_ENABLE_TSAN` 已配置：独立 Linux **GCC** Debug 构建，不与
ASan/UBSan 共用（CMake 配置期拒绝组合）。M1C 验收前必须通过；M2 多 worker
开始前是强制门。

TSan 使用独立 Linux/GCC Debug 构建，不与 ASan、UBSan、LTO、fuzz 或 benchmark
混跑。**本环境的受支持 TSan 编译器是 GCC**：Alpine/musl 的 clang 不发布 TSan
运行时（`libclang_rt.tsan_cxx.a` 不存在），配置期即拒绝并要求改用 GCC
（`-DCMAKE_CXX_COMPILER=g++`）。第一批至少覆盖 HTTP 事件循环与 worker 线程之间
的 command/event handoff、并发 keep-alive、disconnect/cancel、timeout 和
shutdown/reap。任何第一方代码报告均失败；第三方 suppression 必须限定到具体外部
符号、写明原因，不能用宽泛规则隐藏 Host、Runtime 或 IPC 代码。TSan 结果只证明
竞态检测，不作为 QPS、延迟或 CPU 结论。

**已知覆盖缺口（编译诊断降级，不是 race suppression）**：GCC 15.x 的 libstdc++
在 `-fsanitize=thread` 下对 `std::atomic_thread_fence` 报
`-Werror=tsan`（Boost.Asio 的 fenced block 使用该原语）。构建只把该警告类降级为
非致命（`-Wno-error=tsan`，经 `capsid_sanitizers` INTERFACE 继承），TSan 插桩本身
保持开启——已用注入 race 的探针验证仍被检出。代价是 **TSan 不插桩
`std::atomic_thread_fence` 本身**，fence 附近的竞态可能漏检；这属于记录在案的诊断
降级，不影响其他全部同步原语的检测。

TSan 运行环境有硬性要求：Clang TSan 初始化必须调用
`personality(ADDR_NO_RANDOMIZE)` 关闭 ASLR，因此默认 Docker/containerd
seccomp profile 的容器（包括本地 bench/build 容器）会在
`tsan_platform_linux.cpp:282` 直接 CHECK 失败——这不是代码缺陷，是环境不满足
前置条件。TSan 门必须在真实 VM（GitHub hosted runner）或
`--security-opt seccomp=unconfined` 的容器中运行。另外 TSan worker 的 shadow
内存映射要求保留大段虚拟地址空间，sanitizer 构建（`CAPSID_ASAN_BUILD` /
`CAPSID_TSAN_BUILD`）会关闭生产 RLIMIT_AS，仍保留 QuickJS heap 上限。

CI 的 `sanitizers` matrix 新增独立 `tsan` entry：Clang 编译、`CAPSID_BUILD_HOST=ON`，
并在 job 内从 pinned 源码（SHA-256 校验）构建 OpenSSL 3.5 到 `/opt/openssl35`
（ubuntu-24.04 自带 3.0，不满足 Host 的 3.5 契约）。asan/ubsan entry 维持原配置。

**指标开启路径是 TSan 门的另一半**：M1C 验收 A/B 证据在
`CAPSID_HOST_IPC_METRICS=1` 下生成，而 metrics 由 worker 线程写、IO 线程读并
整体清零（`write_metrics_line` 的 `metrics_ = Metrics{}`），无锁跨线程访问是
真实竞态（冻结 RED：`host_single_worker_integration_metrics`）。TSan 只有
metrics-off 通过不够——证据路径同样必须无竞态。`host_single_worker_integration_metrics`
（ctest entry，`CAPSID_HOST_IPC_METRICS=1` 环境）与普通集成测试并列为 M1C 门。

## 平台契约门

平台门分别证明“原生开发”和“生产隔离”，不能互相代替：

- Linux Release 是 v1 生产门，必须在 delegated 环境验证 strict sandbox、
  cgroup 和 required READY feature bits；
- macOS native-dev 运行平台中立 Host 单元测试和真实 single-worker loopback
  集成；strict sandbox 请求必须负控失败；
- Windows native-dev 轨道不在 M1 门内；获得真实 Windows 机器或 hosted runner 后，
  才新增 Windows x64/MSVC job，并在 Windows 宿主真实启动 Host 与 worker，覆盖
  source/trusted-bytecode identity、`capsid:env`、request、streaming、cancel、timeout、
  crash/reap 和 loopback-only 负控；
- Windows 交叉编译、Wine、WSL2 或 Linux 容器不能替代 hosted Windows 原生运行证据；
- Windows/macOS native-dev 通过不得写成生产 sandbox 通过；反之，Linux
  生产门也不能替代 Windows 开发可用性。

Windows 轨道启动时的第一个门必须先保存 RED 证据：当前 POSIX-only Runtime 在 MSVC
构建或 native single-worker 集成处失败；实现不得通过跳过 worker 测试、禁用
trusted bytecode 或把 listener 替换成非原生 Linux VM 来转绿。

## 环境型 sandbox 证据

普通宿主缺少 cgroup delegation 或 namespace 权限时，相应测试返回 CTest
skip code 77。这只表示环境不具备前置条件，不能写成通过。

`.github/workflows/testing-validity.yml` 包含四类独立 job：

- Ubuntu Release/LTO、固定 WPT、benchmark smoke 和 privileged delegated
  sandbox，并生成 txiki.js 升级报告；
- Ubuntu ASan、UBSan 与 TSan 普通矩阵；TSan entry 使用 Clang 并构建
  Host（含本地源码构建的 OpenSSL 3.5）；ASan 仅排除两个已由 Release 严格门禁和
  ASan 非严格同功能门禁重复覆盖的 strict-sandbox 网络/TLS 退出项，因为
  seccomp 会在 instrumented runtime teardown 阶段终止进程；
- Clang/libFuzzer 的四个 bounded corpus gate；
- macOS 14 的 POSIX host-library 与非 worker 单元矩阵。

上述是当前 workflow 事实；它尚未包含 Windows job，因此当前 commit 不声称
Windows 原生开发已交付。这不阻塞 M1，但未来的 Windows native-dev 里程碑在
加入上述 hosted Windows 门之前不得标记完成。

JUnit、build metadata 和升级报告作为 workflow artifact 保存，不在仓库提交生成报告的
副本。最终
`hosted-evidence-index` job 下载各 job 证据，写入 run URL、commit SHA、
各证据文件 SHA-256 和证据树摘要，并要求所有依赖 job 成功。delegated sandbox
脚本把 77 视为 CI failure；普通 runner 的环境型 skip 不能替代它。

所有第三方 action 都固定到审查过的 40 位 commit SHA。仓库内的
`testing_validity_workflow_audit` 同时锁定 action 清单、四类 job、安全门和
CTest JUnit 相对路径；`--test-dir` 已经确定输出根目录，禁止再次把 build
目录写进 `--output-junit`，以免报告与 artifact 读取到不存在的双层路径。

## 当前证据如何取得

测试数量和结果以当前 build tree 为准：

```sh
ctest --test-dir build --show-only=json-v1
ctest --test-dir build --output-on-failure
```

固定 WPT 文件集合由 `tests/wpt/manifest.json` 决定；delegated sandbox 正向测试必须在
具备权限的独立环境中通过，不能用普通宿主的 skip 代替。CI 会为每个 commit 生成
txiki.js 升级报告和 hosted evidence index；这些带 commit 与摘要的 artifact 才是该次
运行的证据，文档不复制某一天的计数。
