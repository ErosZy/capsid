# 合规偏差

目标 profile：`CAPSID-MIN-2025-subset-v0`

规范基线：ECMA-429 第一版，2025 年 12 月。

本表区分主动接受的 profile 排除与已经关闭的实现缺口。开放的实现 bug 不能
当成受支持偏差，也会阻止对应能力的 conformance 声明。

| ID | 能力 | 分类 | 当前行为 | 影响 | 退出条件 |
| --- | --- | --- | --- | --- | --- |
| CAPSID-D001 | `WebAssembly.Tag`、`WebAssembly.Exception`、`WebAssembly.JSTag` | 接受的 profile 排除 | 固定 WAMR/txiki 组合不暴露 exception-handling JS 接口。 | 依赖异常处理 proposal 的 Wasm 不受支持；不得宣称完整 ECMA-429 Wasm conformance。 | 采用具备所需语义的引擎/配置，并通过固定 Wasm JS API 测试。 |
| CAPSID-D002 | WebAssembly 固定宽度 SIMD | 接受的 profile 排除 | `WAMR_BUILD_SIMD=0`；进程契约确认 SIMD module 被 `WebAssembly.validate()` 拒绝。 | SIMD module 验证或编译失败。 | 在不引入失控依赖下载的前提下启用 SIMD，通过固定测试并发布新版 profile。 |
| CAPSID-D003 | Console Standard | 已关闭实现缺口 | 方法名、代表操作和 `console-is-a-namespace.any.js` 均通过。 | 选定批次无已知缺口。 | 2026-07-25 关闭；扩展测试暴露语义缺口时重开。 |
| CAPSID-D004 | `Performance` 接口 | 已关闭实现缺口 | `Performance` 继承 `EventTarget`；branding、`timeOrigin`、`now()`、`toJSON()` 和两个 HR-Time 文件通过。 | 选定批次无已知缺口。 | 2026-07-25 关闭；扩展测试暴露语义缺口时重开。 |
| CAPSID-D005 | 错误与 rejection reporting | 已关闭实现缺口 | `reportError`、`PromiseRejectionEvent`、`unhandledrejection`/`rejectionhandled` identity 与 task ordering 通过。上游 `promise-rejection-events.html` 未执行，因为它依赖 document-scoped harness；manifest 将其列为 `notExecuted`，ordering 由项目 contract 证明。 | 已执行批次无已知缺口；上游 document harness 仍是显式证据缺口。 | 2026-07-25 关闭；支持该执行模型后若暴露语义差异则重开。 |
| CAPSID-D006 | TextDecoder legacy multibyte encoding | 已关闭实现缺口 | GBK、GB18030、Big5、EUC-JP、EUC-KR、ISO-2022-JP、Shift_JIS 由项目标准状态机和压缩 index 实现；选定 decode/stream/EOF/fatal corpus 通过。 | 选定 Encoding 批次无已知缺口。 | 2026-07-25 关闭；扩展 corpus 暴露缺口时重开。 |
| CAPSID-D007 | Compression Streams brotli | 接受的 profile 排除 | 只支持 gzip、deflate、deflate-raw；`brotli` 被拒绝。 | 依赖 Compression Streams brotli 的应用不受支持。 | 在不增加 ambient capability 的情况下实现 brotli，通过固定测试并修订 profile。 |
| CAPSID-D008 | WebAssembly shared memory/threads | 接受的 profile 排除 | `WAMR_BUILD_SHARED_MEMORY=0`；shared Memory 不具备合规 grow 语义，对应精确 WPT 为 expected failure。 | Wasm thread/shared linear memory 应用不受支持；非 shared 行为已通过选定 corpus。 | 启用 WAMR shared memory 和宿主原语，移除 expected failure 并通过固定测试。 |
| CAPSID-D009 | QuickJS Proxy constructor probe | 暂定引擎偏差 | 即使 target 可构造，QuickJS 也会拒绝标准 Proxy-based `IsConstructor` 探针；Encoding IDL harness 使用等价 `Reflect.construct` 并继续检查全部 interface constructor。 | 依赖该 Proxy constructibility pattern 的用户代码观察到非标准行为。 | 修复/升级 QuickJS，恢复原探针并通过固定 IDL harness。 |
| CAPSID-D010 | `MessagePort_initial_disabled` WPT 陈旧 | 接受的 WPT 上游分歧 | 该文件断言新建端口初始 stopped，与当前 WHATWG 规范相反；文件自身也标记可能是未维护重复用例。 | 单个子测试 `Untitled test` 为 expected failure，不影响 profile 能力。 | WPT 上游修正并重新发布后移除 expected-failure 项。 |
| CAPSID-D011 | GB18030-2022 新增码位 | 暂定引擎偏差 | QuickJS/txiki 的 `TextDecoder` 编码表基于 GB18030-2005；GB18030-2022 标准新增的 18 个码位（U+9FB4–U+9FBB 汉字及 U+FE10–U+FE19 竖排标点对应映射）解码结果与 WPT 期望不符。`gb18030-decoder.any.js` 的 18 个子测试（`GB18030-2022 19`–`GB18030-2022 36`）为 expected failure。 | 依赖 GB18030-2022 新增码位解密的文本会得到替代映射字符；GB18030-2005 既有码位全部正确。 | 升级/修复 QuickJS 编码表至 GB18030-2022，移除 expected-failure 项并通过固定 WPT。 |

## 部署资源策略

以下限制会拒绝某些规范上有效的工作负载，因此部署者必须公布，但它们不是新增
JavaScript API：

- Wasm linear memory 最多 256 pages（16 MiB），table 最多 1024 elements；
- `max_fetch_request_body_bytes` / `max_fetch_response_body_bytes` 可限制出站
  Fetch 聚合 body，默认 `0` 表示不增加总量限制；
- strict sandbox、namespace、cgroup、CPU/内存/swap/PID/fd 均可导致启动或
  工作负载失败。

WASI、Hono/Workers 的 `env`/`ExecutionContext`、txiki `tjs:*`、process、
raw socket、HTTP server、FFI、SQLite，以及 Capsid 自己的只读文件模块都属于
产品能力边界，不属于 ECMA-429 合规偏差。
