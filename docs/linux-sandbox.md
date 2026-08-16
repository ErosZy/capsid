# Linux 严格沙箱

> 三平台能力总览与选型建议见[平台支持总览](platform-support.md)。

Linux sandbox 只由宿主通过 C ABI 配置，不会暴露为 JavaScript global、
module 或 permission prompt。

## 严格基线

所有 worker 都会在初始化 txiki.js 前关闭无关继承 fd，并以显式空环境启动。
严格模式还会关闭继承的 stdin/stdout/stderr；应用日志继续走 FetchRPC。bundle
只在 HELLO 校验和沙箱安装完成后才解析。

`strict_sandbox = 1` 要求 `CAPSID_SANDBOX_FEATURE_STRICT_BASE` 全部成功：

- rlimit；
- `no_new_privs`；
- 默认拒绝的 Landlock 文件系统规则；
- seccomp BPF syscall allowlist。

Landlock 只读开放 resolver/hosts 配置、系统 CA、时区数据、内核随机设备和
显式 `tls_ca_bundle_path`。应用 bundle 始终在内存中。

seccomp 允许 txiki.js 标准 `fetch()` 所需的进程内存、event loop、DNS、TLS
和 IPv4/IPv6 stream/datagram 操作；拒绝 listener、Unix/raw socket、
process/thread 创建、exec、ptrace、沙箱安装后的 namespace/mount 变更、文件
系统写入、可执行映射以及 key/BPF/perf 等内核接口。

严格模式目前要求 Linux x86-64/AArch64，以及可用的 Landlock 和 seccomp。
缺少任一强制特性都会启动失败，不能以部分严格模式报告 READY。其他平台请求
严格模式同样 fail-closed。

## 可选隔离

`sandbox_required_features` 可强制要求：

- user namespace；
- private mount namespace；
- IPC namespace；
- UTS namespace；
- cgroup v2 membership；
- 进入宿主预配置的 network namespace。

需要 namespace 时，runtime 会先建立 user namespace；所有 namespace 设置都
发生在 Landlock/seccomp 之前。

### cgroup v2

`sandbox_cgroup_path` 必须是已存在、绝对、已委派的 cgroup v2 目录。宿主负责
创建目录并在父层启用 controller；runtime 不修改
`cgroup.subtree_control`。

ABI v7 的 `capsid_resource_limits.enabled_fields` 区分“未设置”和“显式为零”。
支持：

- `file_descriptors` → `RLIMIT_NOFILE`；
- CPU quota/period → `cpu.max`；
- CPU weight → `cpu.weight`；
- memory high/max/swap max；
- PID max。

`CAPSID_RESOURCE_UNLIMITED` 和 `CAPSID_RESOURCE_PIDS_UNLIMITED` 写入内核的
`max`。runtime 会保存旧值、逐项写入并回读；任一步失败都逆序尽力回滚，并在
HELLO 前验证 child PID 已进入目标 cgroup。目录清理由宿主负责。

### 网络命名空间

`sandbox_network_namespace_fd` 接收宿主已配置好的 Linux network namespace
fd。非负 fd 要求 strict mode，并隐式要求
`CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE`。

runtime 校验 fd 类型，进入 namespace 后比较 inode，并保持调用方 fd
ownership 不变。宿主负责 veth、route、DNS、firewall/NAT 和 namespace
生命周期；runtime 不会替宿主建立网络。

## 出站网络策略

`egress_policy == NULL` 表示 deny-all。rule target 支持：

- 精确 ASCII hostname；
- `*.example.com` 形式的单标签通配；
- 数字 IP；
- canonical IPv4/IPv6 CIDR。

deny 始终优先。即使 `default_action` 为 allow，loopback、link-local、
private/unique-local、metadata-adjacent、multicast、unspecified、documentation
等受保护地址仍需显式 CIDR allow。

策略会检查原始 hostname、DNS 选择后的实际 connect 地址和每次 redirect。
hostname allow 不能绕过 DNS rebinding 防护。若 capability policy 也提供
`net_policy`，两者必须同时允许。

CA bundle、请求/响应 body 上限都属于宿主配置，不暴露给 JavaScript。
证书链和 hostname 验证不会因为自定义 CA 而关闭。

## 明确限制

- 允许标准 Fetch 意味着 seccomp 不能禁止所有 socket syscall；
- Landlock 不是网络边界，network namespace/firewall 才是更强的网络隔离；
- cgroup/namespace 前置条件由部署环境提供，runtime 不获取额外权限；
- strict sandbox 不是多租户调度器，也不替代宿主的 worker 池和审计；
- capability policy 与 OS sandbox 互相补充，不能相互代替。

## 测试与 CI

进程测试覆盖 strict enforcement、fd hygiene、namespace、cgroup controller
写入/回读/回滚、network namespace inode、direct HTTP/HTTPS Fetch 和自定义
CA。

普通宿主缺少 delegation 或 namespace 权限时，相应测试返回 CTest skip 77。
这不是正向证据。hosted validity workflow 会在
`--privileged --cgroupns=private` 容器中运行
`scripts/run-delegated-sandbox-tests.sh`，并把任何 77 当成失败。

完整测试分层和命令见[测试与持续门禁](testing.md)。
