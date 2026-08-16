# 安全策略

Capsid 将应用代码放入独立 worker 进程，但“进程隔离”本身不等于完整安全边界。
是否适合运行不可信代码，取决于 strict sandbox、Host 能力上限、应用权限、网络
namespace/firewall、资源限制与下游网关限制是否同时正确配置。

## 支持范围

项目目前处于 `0.1.x` 阶段，安全修复面向最新代码线；尚不承诺旧版本的长期安全
维护。第一方 `capsid-host` 当前用于开发、集成和 benchmark，不是稳定的生产部署
接口。Linux strict sandbox 是 v1 的生产隔离目标；macOS native-dev、Windows
native-dev 和任何未隔离模式不得用于执行不可信代码。平台差异见
[平台支持总览](docs/platform-support.md)。

以下内容属于项目安全边界：

- Runtime/worker IPC、解析、流控与生命周期；
- capability policy、模块门禁和出站网络判定；
- Linux seccomp、Landlock、rlimit、cgroup 与 namespace 集成；
- bundle、可信 bytecode、build identity 与签名校验；
- 第一方 Host 的配置、Admin API、发布恢复和凭据检查。

部署方的 TLS、外部网关、主机 firewall、secret 生命周期和操作系统加固不由
Capsid 单独保证，但 Capsid 与这些边界的接口缺陷仍欢迎报告。

## 私密报告漏洞

请不要在公开 Issue、Discussion、Pull Request 或 benchmark artifact 中披露尚未
修复的漏洞细节。

优先使用 GitHub 的
[私密漏洞报告](https://github.com/ErosZy/capsid/security/advisories/new)。如果该入口
暂时不可用，请只创建一个不含技术细节的 Issue，请求维护者建立私密联系渠道。

报告中请尽量包含：

- 受影响的 commit、版本、平台和构建选项；
- 前置条件、最小复现步骤与实际影响；
- 是否需要不可信 bundle、特定权限或宿主配置；
- crash、audit、sanitizer 或 sandbox 证据，但不要包含真实 secret；
- 已知缓解措施，以及你期望的协调披露时间。

维护者确认问题后会协调复现、修复、回归测试与披露。项目当前不承诺固定响应 SLA；
在修复和用户迁移窗口就绪前，请避免公开利用细节。

## 部署者最低要求

- 默认 `strict_sandbox` 为关闭状态；默认配置只能运行受信任代码。
- 权限必须从最小集合开始，Host 上限不得使用宽泛 allow 代替应用声明。
- 出站网络同时限制 hostname、解析后 IP/CIDR、redirect 和 OS 网络边界。
- 只有 `CAPSID_EVENT_READY.flags` 满足部署要求后，worker 才能进入调度。
- QuickJS bytecode 不是安全输入格式，只能加载由完全相同构建生成且经宿主校验的
  可信产物。
- 协议错误、worker exit 或同步 CPU timeout 后必须摘除并替换 worker。

完整部署边界见[架构说明](docs/architecture.md)、
[平台支持总览](docs/platform-support.md)、[能力策略](docs/capability-policy.md)
和[Linux 严格沙箱](docs/linux-sandbox.md)。
