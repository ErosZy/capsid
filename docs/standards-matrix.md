# Capsid Runtime 能力追踪矩阵

规范基线为 ECMA-429 第一版（2025 年 12 月），目标 profile 是
`CAPSID-MIN-2025-subset-v0`。一项能力只有在 conformance test 和进程集成测试
都通过时才能标为完成。

| 能力组 | 规范/合规证据 | 进程证据 | 状态 |
| --- | --- | --- | --- |
| 全局表面与 txiki 隔离 | 版本化 profile manifest | `worker_global_surface`、module denial、`worker_p1_platform_contract` | 选定表面与隔离通过 |
| Event 与 rejection reporting | 3 个 EventTarget 文件、`reportError`、PromiseRejectionEvent 与 rejection lifecycle | `worker_p1_platform_contract` | 选定批次通过；CAPSID-D005 已关闭 |
| Timer 与 microtask | 2 个 timer、1 个 `queueMicrotask` 文件 | `worker_p1_platform_contract` | 通过 |
| Encoding | 39 个固定文件，含 Web IDL、stream、legacy multibyte corpus | `worker_p1_platform_contract` | 选定 corpus 通过；CAPSID-D006 已关闭 |
| URL / URLPattern | URL、URLSearchParams、URLPattern constructor | `worker_p1_platform_contract` | 3/3 文件通过，含 893 个 URL constructor case |
| Streams / MessageChannel | 4 个 Streams、4 个 MessageChannel/Port 文件 | `worker_p1_platform_contract` | 8/8 文件通过 |
| Blob / File / FormData | Blob、File constructor | `worker_p1_platform_contract` | 2/2 文件通过；FormData 有进程覆盖 |
| Compression | compression-stream 与固定资源 | `worker_p1_platform_contract` | gzip/deflate/deflate-raw 通过；brotli 为 CAPSID-D007 |
| Console | `console-is-a-namespace.any.js` | `worker_p1_platform_contract` | 通过；CAPSID-D003 已关闭 |
| Web Crypto | getRandomValues、randomUUID | `worker_p1_platform_contract` | 2/2 文件与 digest/random 进程行为通过 |
| Fetch | 5 个 Headers/Request/Response 文件 | direct fetch、cancel、HTTP/HTTPS、egress、netns 测试 | 选定 constructor/WebIDL 与真实出站矩阵通过 |
| Performance | HR-Time `basic`、`monotonic-clock` | `worker_p1_platform_contract` | 通过；CAPSID-D004 已关闭 |
| WebAssembly 子集 | 12 个 compile/instantiate/validate、Memory/Table/Global 与 streaming 文件 | `worker_wasm_minimal` 及 shared/exported resource 回归 | 12/12 文件通过；CAPSID-D001/002/008 为接受的排除 |
| 宿主能力策略（非 conformance 扩展） | capability manifest、module contract、policy/audit/parser 与 fuzz | permissions、utility、env、system、storage、stdio、fs 真实 worker contract，逐模块拒绝矩阵与最终二进制审计 | ABI v7 / policy v2 三层门禁通过；十二个 `capsid:` 模块可逐项授权，已知延后模块和操作保持 `unavailable` |

expected failure 只接受精确 test name 和偏差 ID。标准测试优先于现有 txiki.js
行为；偏差变化必须作为 profile 变更审查。

当前 adapted batch 在独立 realm 中执行 84 个上游文件。它只证明这些固定文件，
不代表全部 WPT 或完整 ECMA-429 conformance。
