# 逃逸级能力门禁

`capsid:ffi` 与 `capsid:raw-socket` 不属于普通 capability。它们可以绕过路径、
DNS、redirect 和逐操作授权，因此当前安全结论是“不提供”，而不是“依赖规则
谨慎开放”。

## 构建边界

- `CAPSID_ENABLE_FFI_CAPABILITY` 与
  `CAPSID_ENABLE_RAW_SOCKET_CAPABILITY` 明确存在且默认 `OFF`；
- 任一开关设为 `ON` 都在 configure 阶段 fail closed，因为项目尚无独立 ABI、
  OS sandbox profile 与完整负控；
- 直接传入 txiki 的 `BUILD_WITH_FFI=ON` 同样被顶层配置拒绝，不能绕过 Capsid
  开关；
- restricted txiki overlay 不打包 FFI、POSIX socket 或相关 bytecode，最终
  worker 还必须通过符号、translation unit 和 module specifier 审计。

## 自动化证据

- `escape_capability_defaults`：检查当前 cache 中两个公开开关均为 `OFF`，且
  txiki FFI 没有暗中启用；
- `escape_capability_configure_negative_controls`：在隔离 build 目录分别尝试
  开启 FFI/raw socket，要求 configure 因预期安全原因失败；
- `worker_binary_audit` 及其负控：证明危险 initializer、translation unit 和
  loader specifier 未进入最终 worker，并证明审计器能捕获注入；
- `worker_sandbox_enforcement`：真实进程验证 strict seccomp/Landlock，包括
  raw socket 拒绝；
- capability manifest 拒绝矩阵：应用导入这两个 `capsid:` module 得到
  `unavailable`，不能靠 allow-list 将其变为可用。

将来若产品确实需要其中任一能力，应新开安全设计和 ABI 版本，至少覆盖库路径
与符号约束、socket family/type/protocol、DNS/redirect 绕过、fd 传递、资源
配额、跨请求/跨租户隔离和独立 OS sandbox。不能把当前 fail-closed 开关改成
“实验性可用”来规避这些前置条件。
