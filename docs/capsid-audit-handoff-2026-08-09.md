# Capsid 整体代码审校最终报告与整改交接

更新时间：2026-08-09  
项目目录：`/Users/zenxo/Documents/capsid`

审校状态：**整体审校已完成；产品整改尚未开始，当前不可发布。**

## 一、任务说明

继续审校当前 Capsid 代码和整体进度。任务重点是功能正确性、生命周期、请求隔离、并发模型、资源边界、性能设计、架构完整性、测试证明力和发布工程。

当前阶段只进行代码审校、运行核验和进度评估，不修改产品源码。所有结论必须以当前工作树和实际运行结果为准。

## 二、审校基线

- 当前提交：`b9fd07a`。
- 源码工作树在审校开始时无改动。
- 已有构建目录：`build-m02`。
- Docker 当前不可用。
- 本轮产生的额外内容仅为构建产物、诊断产物或系统临时文件，不能作为源码已经整改的证据。
- 后续继续工作前，应重新确认 HEAD、工作树和构建目录状态。

## 三、总体判断

Capsid 目前尚未达到正式发布条件。主要发布阻断集中在：

1. Runtime 启动、IPC 收尾和资源生命周期。
2. Managed 数据链路、持久化和激活顺序。
3. Host 连接处理、容量定义和并发模型。
4. C ABI 异常边界、版本协商和错误表达。
5. JS 请求标识及异步上下文传播。
6. 安装、打包、构建身份和 Release CI。
7. 长稳、恢复和完整发布物证明不足。

## 四、首轮已完成审校领域

### 4.1 Runtime 客户端生命周期

已复核 spawn、READY、flush、事件读取、shutdown、terminate、destroy、超时状态和文件描述符继承。

已确认部分路径可能导致：

- 启动状态不完整。
- 输出尾部未完全送达。
- 终止原因表达不足。
- 文件描述符继承范围过大。
- 错误路径资源回收不稳定。
- 请求结束字段可能保留前一事件的数据。

后续整改需要建立统一状态机，并为每个终态定义唯一、可观察、可回归的结果。

### 4.2 Host 与 Managed

已复核 listener、连接处理、请求调度、容量、托管配置、激活、持久化和恢复。

主要结论：

- Managed 数据链路尚未形成完整闭环。
- 配置持久化与激活顺序不满足可靠恢复要求。
- 部分管理开关只停留在配置层，没有约束实际运行路径。
- Host 与 Runtime 对容量参数的解释存在偏差。
- 慢连接可能长期占用关键执行资源。
- 多实例端口和调度语义仍不完整。
- 若干注册表和缓存缺少稳定上限。

### 4.3 配置与有界资源

已复核 header、body、queue、inflight、log、audit、registry 和 buffer 等上限。

主要结论：

- 部分注册表和缓存仍可能持续增长。
- 若干默认值与文档不一致。
- Host 与 Runtime 对同一容量参数的解释存在偏差。
- 个别缓冲区只移动逻辑前缀，物理内存不能及时收缩。
- 部分限制只存在于配置对象，没有在实际路径形成完整闭环。

### 4.4 CI、测试和文档证明力

已复核平台矩阵、严格编译、长稳、压力、恢复、文档审计和 benchmark。

主要结论：

- 部分检查只证明文本存在，不能证明功能路径已经接通。
- Linux Release 没有完整构建并验证 Host 与 worker 组合。
- 部分平台组合缺少严格编译验证。
- 长期资源变化、连续取消、队列饱和、公平性和恢复覆盖不足。
- 部分 benchmark 未覆盖完整 Host 到 Worker 路径，不能直接用于容量承诺。
- 局部组件通过不能替代完整运行链路的验收。

### 4.5 能力规则、运行隔离与性能口径

已复核能力判断、审计归属、文件、网络、标准流入口、资源计量、基准路径和恢复接线。

主要结论：

- 异步任务的请求归属不完整。
- 部分日志和审计事件可能得到请求编号 0。
- 若干能力入口依赖单一全局请求编号。
- 部分性能结果不代表完整 Host 到 Worker 路径。
- 当前 benchmark 不能直接作为正式容量承诺。
- 恢复接线和运行状态转换仍有缺口。

### 4.6 压力、计量、长稳与故障模拟

现有测试覆盖了部分边界，但尚不能证明完整长稳目标。

需要补充：

- 长期资源增长。
- 队列持续饱和后的公平性。
- 连续取消与连续超时。
- 进程替换和恢复。
- 多请求交错下的状态清理。
- Host 和 Runtime 双层期限同时生效时的行为。
- 指标采集本身对性能的影响。

### 4.7 证明材料与签名材料生命周期

主要结论：

- 可信键没有完整接入主运行路径。
- 轮换与恢复顺序可能使已有状态不可读。
- 文件权限和重复项检查不完整。
- 内存清理策略不充分。
- 跨平台产物读取仍存在不稳定测试。
- 配置允许项与实际使用项之间缺少完整一致性验证。

### 4.8 C ABI 与错误模型

主要结论：

- C++ 异常可能越过 C 接口边界。
- `struct_size` 和版本协商当前更多是形式检查。
- 未识别事件的首轮行为待复核；最终确认会返回协议错误，见第 12.3 节。
- 超时数值转换存在边界不一致。
- EXIT 事件可能携带陈旧字段。
- 错误文本不足以支持稳定诊断。
- 某些大输入在进入核心处理前会进行一次性分配。

后续整改应要求所有公开 C 函数捕获内部异常，并将结果转换为稳定的 `capsid_result` 与可查询错误详情。

### 4.9 安装、打包、构建身份与发布物

已实际确认：

- 顶层没有有效的安装规则，安装命令成功但不安装实际内容。
- 顶层 package 目标被依赖项目接管。
- 当前会生成名称错误、内容为空的压缩包。
- 构建身份没有覆盖关键编译选项、源码状态和完整工具链。
- 普通构建、部分检查构建和不同内存分配器配置可能得到相同身份。
- Release CI 默认没有启用 Host。
- 缺少可核验的完整发布物与物料清单链路。
- Host 依赖缺失时可能被静默跳过。

发布整改至少需要覆盖：

1. 明确的 install 目标和安装目录。
2. Capsid 自己拥有的 package 配置。
3. Host、worker、公共头文件、库、许可证和文档的发布清单。
4. 构建身份覆盖 Capsid 源码、依赖版本、工具链和关键选项。
5. Linux Release 同时构建并运行 Host 与 worker 测试。
6. 对最终压缩包进行内容、名称和可运行性检查。

## 五、JS 请求生命周期首轮发现

重点文件：

- `js/bootstrap.js`
- `src/worker_runtime.cc`
- `include/capsid/runtime.h`
- `docs/embedding-api.md`
- `docs/host-integration.md`

### 5.1 已确认：64 位请求标识进入 JS 后失去完整区分能力

C API 和 IPC 使用 `uint64_t`，但 Runtime 使用 `JS_NewInt64` 传给 JavaScript，JS 侧使用普通 Number 作为 Map 键。

相邻的大整数请求标识在 JS 层会变成同一个值。实际并发测试已经失败，而协议层单独测试只证明了 64 位数在进入 JS 之前能够往返。

相关位置：

- `src/worker_runtime.cc:3209`
- `src/worker_runtime.cc:3274`
- `src/worker_runtime.cc:3381`
- `js/bootstrap.js:410`
- `js/bootstrap.js:427`

可选整改方向：

- 在 JS 层使用 BigInt。
- 使用规范化字符串键。
- 拆成两个 32 位字段。
- 或在公开 C 接口明确限制为 JS 可精确表达的范围，并在客户端入口立即拒绝超范围值。

不能继续维持“公开接口接受完整 `uint64_t`，JS 内部却使用普通 Number”的混合契约。

### 5.2 已确认：请求上下文只覆盖处理器第一次同步调用

`capsidEnterRequest(id)` 在调用 handler 前执行，handler 返回 Promise 后立即在 `finally` 中执行 `capsidLeaveRequest(id)`。

因此 `await` 后的 Promise 续段运行时，请求上下文已经变成 0。通用任务队列执行函数没有恢复请求归属。

相关位置：

- `js/bootstrap.js:454-461`
- `src/worker_runtime.cc:683-714`
- `src/worker_runtime.cc:2984-2992`
- `src/worker_runtime.cc:4079`

受影响范围包括：

- 请求期限检查。
- 日志事件请求编号。
- 能力判断记录。
- 文件、标准流和网络操作归属。
- 取消后的续段处理。

### 5.3 已确认：异步续段没有受到请求期限完整约束

已完成有界运行验证：

- 请求期限配置为 50ms。
- handler 先执行一次 `await Promise.resolve()`。
- 恢复后执行约 250ms 的有限计算。
- 最终仍返回 HTTP 200。
- 整体运行时间约 330ms。

原因是运行期限检查依赖 `executing_request_id_`，而异步恢复时该字段为 0。

相关位置：

- `src/worker_runtime.cc:425-443`
- `src/worker_runtime.cc:431-440`
- `src/worker_runtime.cc:4079`

### 5.4 已确认：取消后的延迟续段仍会影响下一请求

已完成有界运行验证：

- 请求 301 启动一个 75ms 延迟续段。
- 约 20ms 后完成取消流程。
- 延迟续段随后仍恢复并修改模块共享状态。
- 请求 302 读到修改后的值 `1`。

这与文档中“取消传播到 handler”“异步期限后 worker 可以继续复用”的表述不完全一致。取消当前只能终止部分受 AbortSignal 管理的操作，不能阻止普通 Promise 或定时器续段继续执行。

相关位置：

- `js/bootstrap.js:585-609`
- `docs/embedding-api.md:69-71`
- `docs/embedding-api.md:91-94`

### 5.5 首轮待确认项：终态记录淘汰顺序（最终降为 P2）

`terminal_requests_` 使用 `std::set<uint64_t>`。超过 2048 项时删除 `begin()`，因此删除的是数值最小项，而不是完成时间最早项。

相关位置：

- `src/worker_runtime.cc:69-74`
- `src/worker_runtime.cc:3304-3318`
- `src/worker_runtime.cc:4066-4068`

代码注释隐含依赖 Host 请求编号严格递增，但公开 C 接口只要求请求进行期间唯一，没有统一要求所有调用方永久递增。

后续已完成低编号插入后的存活请求，worker 继续正常处理；最终结论和证据见第 10.1
节。该项已降级为实现前提与可维护性问题。

### 5.6 首轮待确认项：取消后的 Promise 内存（最终已排除）

静态代码能够证明取消后某些异步任务仍可能存在，但这不能证明对应 Promise 环无法被 QuickJS 回收。

核验使用 `CAPSID_EVENT_MEMORY_METRICS` 完成以下指标对比：

1. 记录空闲基线。
2. 多轮创建并取消永久等待请求。
3. 采集对象数、属性数和实际内存。
4. 主动执行回收。
5. 再次采集指标。
6. 至少重复三轮。

三轮共 1536 次取消后的指标已收敛，最终结论和数值见第 10.2 节；本轮有界测试未
发现持续内存增长。

## 六、详细执行计划（已完成）

### 阶段 A：完成 JS 生命周期核验

1. 完成终态记录测试，并增加后续存活请求。
2. 使用内存指标区分可回收对象与真实持续增长。
3. 验证 Promise 续段中的日志事件请求编号。
4. 验证 Promise 续段中的能力判断记录请求编号。
5. 验证并发 Promise 交错时上下文是否串到另一请求。
6. 验证取消和期限结束后的任务是否仍能使用请求级能力入口。
7. 检查所有现有框架兼容测试是否只验证 HTTP 结果，而没有验证请求归属。

### 阶段 B：设计 JS 生命周期整改方案

整改方案需要满足以下不变量：

1. 请求标识在 C、IPC、C++ 和 JS 四层保持一致。
2. 每个 Promise 任务执行前恢复对应请求上下文。
3. Promise 任务结束后恢复先前上下文。
4. 取消或期限结束后，所有请求级入口拒绝继续使用已结束上下文。
5. 已取消任务不能影响后续请求的请求级状态。
6. 日志、审计、文件、标准流和网络事件必须带正确请求编号。
7. 若无法可靠停止异步续段，应把 worker 标记为不可继续复用，或采用每请求独立执行单元。

可研究 QuickJS Promise Hook，或在框架桥接层维护 Promise 与请求编号之间的映射。不能继续依赖单一全局 `executing_request_id_` 覆盖整个异步生命周期。

### 阶段 C：整理自动化覆盖差距

逐项查找并补充测试规格：

- 64 位请求标识边界。
- `await` 前后请求归属。
- 异步续段期限。
- 取消后的延迟续段。
- 取消后跨请求共享状态变化。
- 多请求 Promise 交错。
- 日志和能力记录归属。
- 终态记录的非单调编号。
- 多轮取消后的内存指标。
- 安装目录实际内容。
- 最终压缩包内容。
- 构建身份唯一性。
- Linux Release Host 与 worker 组合。

### 阶段 D：形成最终整改路线

按以下优先级整理：

#### P0：发布阻断

- Runtime 生命周期和 IPC 收尾。
- JS 异步上下文和请求期限。
- C ABI 异常边界。
- Managed 数据链路闭环。
- 安装和打包目标。
- 构建身份唯一性。

#### P1：高优先级

- Host 并发与容量定义。
- 慢连接处理。
- 配置持久化与恢复。
- Release CI 完整矩阵。
- 注册表和缓存上限。
- 签名材料轮换与恢复。
- 长期资源指标。

#### P2：工程改进

- 性能测试口径。
- 文档一致性。
- 错误文本和诊断数据。
- 终态记录容器语义。
- 测试命名、维护性和重复逻辑清理。

建议整改顺序：

```text
Runtime 正确性
→ JS 请求生命周期
→ Host 与 Managed 闭环
→ C ABI 与错误模型
→ 安装、构建身份和发布物
→ 长稳与完整矩阵
→ 性能优化
```

## 七、最终报告格式

每个条目必须包含：

1. 优先级。
2. 文件和行号。
3. 当前行为。
4. 直接证据。
5. 影响范围。
6. 目标不变量。
7. 建议改动范围。
8. 回归测试。
9. 验收条件。
10. 当前状态：已确认、静态推断、证据不足或已排除。

## 八、审校纪律

- 只报告有源码或运行结果支持的结论。
- 静态推断与运行确认必须分开标注。
- 一次通过不能证明长期稳定。
- 文档字符串检查不能证明实现完整。
- 单组件测试不能证明完整运行链路。
- 未完成指标对比前，不得宣称存在持续内存增长。
- 未完成后续存活验证前，不得把终态记录问题写成确定结论。
- 不修改产品源码，除非用户后续明确要求实施整改。
- 最终报告完成后，新增结论必须以新的提交基线和可复现证据单独追加。

## 九、计划完成状态

- 基线与提交复核：完成。
- Runtime 生命周期：完成首轮。
- Host、Managed 和配置：完成首轮。
- CI、测试与文档：完成首轮。
- 能力规则、运行隔离和性能口径：完成首轮。
- 压力、计量、长稳和故障模拟：完成首轮。
- 证明材料与签名材料生命周期：完成首轮。
- C ABI 与错误模型：完成首轮。
- 安装、打包、构建身份和发布物：完成首轮。
- JS 请求标识与异步生命周期：阶段 A 核验完成，阶段 B 整改设计完成。
- JS 剩余核验：完成。
- 覆盖差距整理：完成。
- 最终整改路线：完成，见第十二节。
- 完成审计：完成。

## 十、2026-08-09 阶段 A 续审结果

本节基线仍为 `b9fd07a`。产品源码没有修改。诊断源码、诊断 worker 和输出只位于
`build-m02-probe`，不构成产品整改。

### 10.1 P2：终态记录的非单调编号未造成后续存活失败

1. **优先级**：P2。
2. **文件和行号**：`src/worker_runtime.cc:3304-3318`、`src/worker_runtime.cc:4066-4068`。
3. **当前行为**：`terminal_requests_` 是按数值排序的 `std::set<uint64_t>`；超过
   2048 项时删除数值最小项，而不是完成时间最早项。
4. **直接证据**：顺序完成请求 `100000..102047` 填满 2048 项，再完成低编号请求
   `7`，随后请求 `102048` 仍得到 HTTP 200 和正常 RESPONSE_END。诊断程序退出码为
   0。
5. **影响范围**：当前未复现 worker 失活、协议错误或后续请求失败；风险限于实现
   依赖“请求编号永久递增”的隐含前提及未来晚到帧语义维护。
6. **目标不变量**：淘汰策略应按完成时序表达，并且不能依赖公开 C API 未规定的
   编号单调性。
7. **建议改动范围**：将成员关系与完成顺序分离，例如 `set + deque`，或明确并验证
   Host/C API 的永久单调编号契约。
8. **回归测试**：保留 2048 个高编号终态，完成一个低编号请求，再发送后续存活
   请求；另加真实晚到 body/end 帧覆盖。
9. **验收条件**：非单调编号和晚到帧组合不会关闭 worker，且容器始终有界。
10. **当前状态**：运行未复现，降级为实现前提与可维护性问题。

### 10.2 已排除：取消后的永久等待 Promise 未表现为持续内存增长

1. **优先级**：不列发布阻断。
2. **文件和行号**：`js/bootstrap.js:454-468`、
   `src/worker_runtime.cc:3104-3139`。
3. **当前行为**：永久等待 handler Promise 在请求取消后没有完成，但仅凭静态对象
   关系不能判断 QuickJS 是否可回收。
4. **直接证据**：同一 worker 内执行三轮，每轮 512 次“确认 handler 已进入并创建
   永久等待 Promise，然后取消”。每轮成对采集 GC 前/后
   `CAPSID_EVENT_MEMORY_METRICS`；诊断 worker 仅在偶数次快照前调用 `JS_RunGC`。
   基线为 `used=1289688, objects=1712, properties=5250`。第一轮后为
   `used=1291647, objects=1730, properties=5286`，第二、三轮保持完全相同。
   每轮 GC 前后也完全相同。独立复跑得到逐字段相同结果。
5. **影响范围**：本测试规模共 1536 次取消；不能外推为无限时长证明，但足以排除
   “每次取消都保留一个不可回收 Promise 环”的判断。
6. **目标不变量**：多轮取消后的 heap 指标应收敛，不随累计取消次数线性增长。
7. **建议改动范围**：当前无需以“Promise 持续泄漏”为由修改产品；长期 soak 中
   继续保留 heap 指标门限。
8. **回归测试**：将本轮三波指标方案转成诊断/长稳测试，记录基线、波次和 GC 后
   差值。
9. **验收条件**：暖机后多轮的 GC 后对象、属性及实际内存保持平台化。
10. **当前状态**：在本轮有界规模内已排除持续增长。

### 10.3 P0：`await` 后日志与权限审计请求编号稳定丢失为 0

1. **优先级**：P0。
2. **文件和行号**：`js/bootstrap.js:454-461`、
   `src/worker_runtime.cc:1510-1518`、`src/worker_runtime.cc:2194-2210`、
   `src/worker_runtime.cc:4079`。
3. **当前行为**：handler 返回 Promise 后同步执行 `capsidLeaveRequest(id)`；后续
   Promise 任务执行时 `executing_request_id_` 为 0。权限 query audit、stdio
   operation audit 和 LOG frame 都直接读取该单一字段。
4. **直接证据**：请求 41 在一次 `await Promise.resolve()` 后执行权限 query 和
   stdout；并发请求 42/43 分别经 20ms/0ms timer 交错后执行相同操作。三条 LOG、
   三条 query audit event 及其三条 decoded audit record 的 request ID 全部为 0，
   但三个 HTTP RESPONSE_END 的传输 ID 正确保持 41、42、43。
5. **影响范围**：日志归属、能力判断审计、文件、标准流、存储和网络等所有读取
   `executing_request_id_` 的请求级入口；并发调查和租户归责均可能丢失关联。
6. **目标不变量**：每个 Promise 任务执行前恢复所属请求 ID，执行后恢复先前 ID；
   C、IPC、C++ 与 JS 的标识必须一致。
7. **建议改动范围**：QuickJS job/Promise 上下文传播、请求终态注册表、所有请求级
   native 入口的统一校验；不能只包裹 handler 的首次同步调用。
8. **回归测试**：覆盖一次 microtask、timer、嵌套 Promise、两个请求交错，以及
   LOG/AUDIT record 与 transport ID 的逐项一致性。
9. **验收条件**：上述九类事件全部携带原请求 ID，且任务退出后恢复先前上下文。
10. **当前状态**：已运行确认。

### 10.4 P0：取消或期限终态后的续段仍可使用请求级能力入口

1. **优先级**：P0。
2. **文件和行号**：`src/worker_runtime.cc:425-443`、
   `src/worker_runtime.cc:1510-1518`、`src/worker_runtime.cc:2194-2223`、
   `js/bootstrap.js:454-468`。
3. **当前行为**：取消和期限会结束 Runtime 的 response state，但普通 timer/Promise
   续段仍可运行。由于续段身份已变成 0，入口没有识别它属于一个已结束请求。
4. **直接证据**：请求 51 同步日志 ID 为 51；取消后 80ms 续段成功执行权限 query
   和 stdout，日志 ID 为 0。请求 52 同步日志 ID 为 52；先收到 timeout 事件，
   80ms 续段仍成功执行同样入口，日志 ID 为 0。之后请求 53 正常完成，证明 worker
   被继续复用。
5. **影响范围**：终态后的日志、审计、文件、存储、标准流和网络副作用；取消/期限
   不再是请求级副作用边界。
6. **目标不变量**：终态请求的后续任务必须在任何请求级 native 入口处 fail closed；
   若无法可靠阻止普通续段，则 worker 不得继续复用。
7. **建议改动范围**：任务到请求的稳定映射、终态 generation/token、native 入口
   统一门禁，以及必要时 worker 污染/替换策略。
8. **回归测试**：取消后 timer、期限后 timer、终态后 permissions/stdio/fs/storage/
   fetch，以及终态后的 worker 复用。
9. **验收条件**：所有终态续段均不能产生外部或共享状态副作用；拒绝结果可观察且
   不误伤其他活跃请求。
10. **当前状态**：已运行确认。

### 10.5 P1：框架 lifecycle 测试不证明 Capsid 请求归属

1. **优先级**：P1（测试证明缺口）。
2. **文件和行号**：`tests/test_framework_worker_driver.cc:385-467`、
   `tests/test_framework_worker_driver.cc:770-827`、`tests/hono/lifecycle.mjs:96-177`、
   `tests/itty-router/lifecycle.mjs:138-259`、`tests/h3-v2/lifecycle.mjs:50-229`。
3. **当前行为**：共享 driver 验证 REQUEST_CREDIT、HTTP response、error/timeout 和
   EXIT；LOG/AUDIT 被读取后没有断言。框架 `assertCleanContext` 只检查框架自身的
   request-local store。
4. **直接证据**：`worker_hono_lifecycle`、三个 itty-router lifecycle 和
   `worker_h3_v2_lifecycle` 共五项全部通过；同一构建上的独立探针同时确认 await 后
   LOG/AUDIT ID 全为 0。
5. **影响范围**：现有“并发、取消、timeout、reuse”通过不能作为 Capsid native
   请求身份传播正确的证明。
6. **目标不变量**：框架兼容测试除 HTTP 语义外，还必须验证关键 native 事件归属。
7. **建议改动范围**：扩展共享 driver 收集并输出 LOG/AUDIT，给 framework fixture
   增加 await 前后 native 操作，断言每个事件的 request ID。
8. **回归测试**：三个框架至少各有 microtask、timer、并发交错、取消后续段和期限
   后续段一组归属断言。
9. **验收条件**：若任何事件为 0、属于另一请求或终态后仍成功，框架 lifecycle
   测试必须失败。
10. **当前状态**：静态与运行组合确认。

### 10.6 阶段 A 结论

- 阶段 A 七项核验全部完成。
- 新确认两个 P0：异步事件归属丢失；终态后请求级能力入口仍可用。
- 终态记录项降级为 P2 实现前提。
- 永久等待 Promise 的持续内存增长在本轮规模内已排除。
- 下一步进入阶段 B：选择并验证 QuickJS 任务级上下文传播和终态门禁方案。

## 十一、阶段 B：JS 生命周期整改设计结论

### 11.1 64 位请求标识：统一使用 BigInt

建议将 C++ 到 JS 的所有请求 ID 从 `JS_NewInt64` 改为 `JS_NewBigUint64`，JS 的
`requests` Map 使用 BigInt 键，JS 到 C++ 的所有桥接入口使用
`JS_ToBigUint64`。需要一次性覆盖 begin、chunk、end、cancel、response head/body/end、
credit 和 enter/leave，禁止同一路径混用 Number、BigInt 和字符串。

选择 BigInt 而不是字符串的原因：QuickJS 当前已经提供精确的
`JS_NewBigUint64`/`JS_ToBigUint64`，能保持公开 `uint64_t` 契约，不需要重复定义十进制
规范化、前导零和解析失败语义。回归必须覆盖 `2^53-1`、`2^53`、`2^53+1`、
`2^64-1` 以及相邻大整数并发。

### 11.2 现有 Promise Hook 不能单独承担上下文传播

静态核验结果：

- `vendor/txiki.js/deps/quickjs/quickjs.h:1116-1131` 暴露 INIT、BEFORE、AFTER、
  RESOLVE 四类 Promise Hook。
- INIT 可以得到 `parent_promise`，适合建立 Promise 血缘映射。
- 普通 Promise reaction 在 `quickjs.c:54420-54458` 和
  `quickjs.c:55170-55249` 中直接通过 `JS_EnqueueJob` 入队；
  `quickjs.c:2175-2201` 执行 `JSJobEntry` 时没有调用 Promise BEFORE/AFTER Hook。
- 当前 BEFORE/AFTER 位于 thenable job 的 `JS_Call` 周围，不是所有 Promise reaction
  的通用执行边界。
- timer 更不经过 Promise job：`vendor/txiki.js/src/timers.c:78-95` 从 libuv callback
  直接调用 `tjs_call_handler()`。txiki 源码中共有约 40 个 `tjs_call_handler()` 调用点，
  分布于 timer、HTTP client、stream、webcrypto、TLS 等原生异步资源。

因此，“只安装 `JS_SetPromiseHook` 并维护 Promise→request ID Map”不能覆盖 timer
callback，也不能为所有 reaction 提供可靠的 enter/leave 执行边界。

### 11.3 建议的任务上下文模型

定义不可复用的请求 token，而不是只保存裸 ID：

```text
RequestToken = { request_id: uint64, generation: uint64, state: active|terminal }
```

需要两个接线层：

1. **QuickJS job 层**：在 job 入队时捕获当前 token，把 token 存入 `JSJobEntry`；在
   `JS_ExecutePendingJob` 调用 job 前进入 token，结束后恢复先前 token。该能力应以
   狭窄的 Capsid/QuickJS overlay hook 实现，不能依赖全局裸 ID。
2. **txiki 原生异步资源层**：timer、fetch/HTTP、stream、webcrypto 等资源在注册时
   捕获 token，在 callback 前进入、callback 后恢复；资源终止时释放 token 引用。
   优先在受限 Capsid 构建实际暴露的模块集合中逐项接线和枚举验证，不能用“修改
   `tjs_call_handler` 一个函数”代替注册时归属，因为 callback 执行时必须知道创建它的
   token。

所有 native 请求级入口必须经过同一个门禁：token 缺失或 token 已 terminal 时拒绝
执行，日志和审计不能悄悄降级为 ID 0。

### 11.4 终态后的 worker 复用策略

任务上下文传播可以阻止终态续段继续使用 native 能力，但不能阻止任意纯 JavaScript
续段修改同一 realm 的模块共享对象。已有运行证据已经证明取消续段能够影响后续请求。

因此 P0 建议采用保守策略：

- 任何请求发生 cancel 或 timeout，立即把 token 标记为 terminal 并取消所有可枚举的
  原生异步资源。
- 当前 worker 标记为不可接收新请求；完成 IPC 收尾后由 Host 替换。
- 如果正常响应结束时仍存在属于该 token 的 detached job/resource，也同样标记 worker
  不可复用。若产品需要合法后台任务，应设计显式、受期限约束的 `waitUntil` 契约，
  不能把任意悬空 Promise 当作后台任务。
- 在证明任务图能够完整枚举和纯 JS 续段无法跨请求写共享状态之前，不应仅凭
  AbortSignal 已触发就复用 worker。

长期可选方案是每请求独立 JSRuntime/realm；隔离最强，但会改变模块缓存、worker 级
共享状态和性能模型。当前更小的正确性闭环是“正常请求可复用，cancel/timeout/
detached-task 后替换 worker”。

### 11.5 实施顺序

1. BigInt 请求 ID 全链路改造及边界测试。
2. 引入 RequestToken 和 native 入口统一门禁。
3. 为 QuickJS job queue 增加 capture/enter/leave overlay hook。
4. 枚举受限构建中的 txiki 异步资源并接线 token；先覆盖 timer 和 direct fetch。
5. cancel/timeout/detached-task 后 worker poison 与 Host replacement。
6. 扩展共享 framework driver，强制验证 LOG/AUDIT/能力入口归属。
7. 完成并发、取消、期限、替换、恢复和资源平台化长稳测试。

### 11.6 阶段 B 验收门

- 64 位 ID 在 C、IPC、C++ 和 JS 四层逐位一致。
- microtask、timer、fetch、stream callback 的 LOG/AUDIT 均携带原请求 ID。
- 任务嵌套执行后恢复先前 token，不串到另一个并发请求。
- terminal token 的所有 native 入口 fail closed，不产生 ID 0 事件。
- 取消、timeout 和 detached task 后，旧 worker 不处理下一请求；替换 worker 可正常服务。
- 旧 realm 中的延迟续段不能影响替换 worker 的模块或请求状态。
- QuickJS/txiki overlay 变更进入构建身份、Release 产物和回归矩阵。

## 十二、整体审校终结报告

### 12.1 审校边界与最终结论

本报告以提交 `b9fd07aa9886755e0cdbf4544b16a8932e93116c` 为源码基线。审校覆盖
Runtime、worker/QuickJS 桥接、Host 单 worker 与静态池、Managed/Admin、配置与能力
策略、IPC、C ABI、构建身份、安装打包、CI、框架兼容测试、压力和诊断证明。

**审校工作已结束，但 Capsid 尚未达到发布条件。** 当前有七组 P0 发布阻断；Linux
严格隔离、WPT 和最终发行包验收属于整改后的外部验收门，不再视为“审校尚未完成”。
本轮没有修改任何产品源码。

### 12.2 最终基线复验

- `cmake --build build-m02 -j2`：全部现有目标构建成功。
- `ctest --test-dir build-m02 --output-on-failure -j2`：共 204 项，150 通过、53
  失败、1 跳过，耗时约 40.38 秒。
- 53 个失败不能直接解释为 53 个产品缺陷。其中 43 项明确报告 macOS 不提供 strict
  sandbox；另有 6 个 Managed 用例在真实 worker 建立或激活前失败，需要 Linux 环境
  才能区分后续功能结果。
- 其余 4 项分别是：`host_artifact_safe_read` 的 macOS socket 错误码差异、审计报告
  尚未加入 `docs/README.md` 导致的文档索引检查、主动设置的 WPT 未配置失败门，以及
  Apple `strip` 不支持 GNU `--strip-all`。
- `worker_cpu_affinity` 是预期的平台跳过；CPU affinity 需要 Linux。
- 工作树中的唯一源码树差异仍是本审校报告本身，状态为未跟踪文件；构建和诊断产物
  位于已忽略的构建目录。

### 12.3 对首轮宽泛判断的校正

后续源码和运行证据对若干首轮结论作了收窄：

- Managed 的发布、恢复、容量许可和 Admin 异步队列已有实际接线；准确缺口是
  `listeners`、`trustedBytecodeKeys` 未映射到主程序，且 Managed 模式没有创建数据面
  server，而不是“整个 Managed 协调器都未接线”。
- `capsid_worker_spawn()` 返回后再 load bundle、等待 READY 符合现有公开文档，不把
  异步启动本身列为缺陷。
- 未知 IPC event 在 `src/client.cc:786-787` 返回 `CAPSID_PROTOCOL_ERROR`，不再保留
  “未知事件被静默忽略”的旧判断。
- 1536 次取消的三波 GC 指标已经平台化，未发现每次取消都造成永久 Promise 持续
  增长；该项不列发布阻断。
- 终态 tombstone 的数值顺序淘汰未造成后续请求失活，降为 P2 实现前提问题。

### 12.4 P0 发布阻断清单

#### P0-1：完整 `uint64_t` 请求编号在 JS 层发生碰撞

1. **位置**：`src/worker_runtime.cc:3209,3274,3381`，`js/bootstrap.js:427`。
2. **行为与证据**：C/IPC 接受 `uint64_t`，桥接使用 `JS_NewInt64`，JS Map 使用
   Number；相邻超出安全整数范围的请求在实际并发探针中发生碰撞。
3. **影响**：请求状态、取消、body、response 和审计可能关联到错误请求。
4. **整改与验收**：按第 11.1 节将全链路改为 BigInt，并覆盖 `2^53-1`、`2^53`、
   `2^53+1`、`2^64-1` 和相邻大整数并发。
5. **状态**：运行确认。

#### P0-2：异步任务没有稳定请求上下文

1. **位置**：`js/bootstrap.js:454-461`，`src/worker_runtime.cc:1510-1518`、
   `src/worker_runtime.cc:2194-2210`、`src/worker_runtime.cc:4079`。
2. **行为与证据**：请求 41/42/43 的 microtask 和 timer 续段所产生的 LOG、audit
   event、decoded audit record 均为请求 ID 0，而 HTTP 传输终态仍保持原 ID。
3. **影响**：日志归属、权限判定审计、文件、标准流、存储和网络入口失去请求身份。
4. **整改与验收**：采用第 11.3 节 RequestToken、QuickJS job 和 txiki 原生异步资源
   双层传播；所有 native 入口统一 fail closed，框架测试必须断言 LOG/AUDIT ID。
5. **状态**：运行确认。

#### P0-3：取消/期限终态不能阻止续段副作用

1. **位置**：`src/worker_runtime.cc:425-443,1510-1518,2194-2223`，
   `js/bootstrap.js:454-468`。
2. **行为与证据**：取消请求 51、超时请求 52 后的 80ms 续段仍能执行权限 query 和
   stdout；另一个探针中取消请求的延迟续段修改模块共享状态，下一请求读到该修改。
3. **影响**：取消和期限不是可靠的副作用边界，复用同一 realm 会造成跨请求污染。
4. **整改与验收**：按第 11.4 节将 terminal token 封禁；在不能完整证明任务图隔离
   前，cancel、timeout 或 detached task 后 poison 并替换 worker。
5. **状态**：运行确认。

#### P0-4：Managed 配置与数据面没有形成可服务闭环

1. **位置**：`src/host/main.cc:212-224,326-543,775-918`，
   `src/host/managed_host.h:17-37`。
2. **行为与证据**：权威 schema 接受 `listeners` 和 `trustedBytecodeKeys`，但
   `ManagedConfig` 和 `parse_managed_config()` 不提取二者；构造
   `ManagedHostOptions` 时也未填充 `trusted_keys`。Managed 模式只保存裸
   `capsid_worker*` pool 并启动 `AdminService`，没有构造 `SingleWorkerServer`、
   `StaticPoolServer` 或任何 TCP data listener。
3. **影响**：配置了 listener 的托管部署仍不能提供数据面；配置的可信字节码键也
   无法进入主运行路径。
4. **整改与验收**：建立 listener/routing 到每 App active pool 的拥有关系；原子切换
   新 generation、drain 旧 pool；安全读取并映射可信公钥。用真实 Managed executable
   验证 deploy 后 HTTP 可达、替换无错路由、retire 停止服务、恢复后重新可达。
5. **状态**：静态确认；完整执行验收需 Linux strict sandbox。

#### P0-5：公开 C ABI 没有 C++ 异常封口

1. **位置**：`src/client.cc:919-1997` 及其他 `extern "C"` 实现；代表路径
   `src/client.cc:985-1374`。
2. **行为与证据**：公开函数内部构造 `std::string`、`std::vector`、`std::map` 和策略
   对象，队列路径也会分配；公开 C ABI 实现中没有 `try/catch`。结果枚举仅有
   `include/capsid/runtime.h:53-61` 的七类结果，也没有稳定的 OOM/内部异常详情。
3. **影响**：分配失败或内部 C++ 异常可越过 C 边界，C 调用方不能得到稳定错误，
   进程可能终止。
4. **整改与验收**：为每个公开入口设置 `noexcept` 边界包装，统一映射 OOM 和内部
   异常；增加 fault-injection allocator 测试，要求异常不逃逸且已分配资源被回收。
5. **状态**：静态确认。

#### P0-6：安装和发行包目标不可用

1. **位置**：顶层 `CMakeLists.txt`；除第三方目录外没有 Capsid `install()` 规则，也
   没有 Capsid 自有 CPack 配置。
2. **行为与证据**：`cmake --install build-m02 --prefix <temp>` 返回成功但临时前缀
   完全为空；`cmake --build build-m02 --target package` 生成
   `libwebsockets-4.5.99-v4.5.0-481-gd32905645-Darwin.tar.gz`，不是 Capsid 发行包。
3. **影响**：无法交付 Runtime、worker、Host、公共头文件或许可证，package 成功也
   会给发布系统错误的成功信号。
4. **整改与验收**：Capsid 顶层拥有 install/CPack；从全新前缀安装并从最终压缩包
   解压运行 smoke test，核对名称、版本、文件清单、权限、依赖和许可证。
5. **状态**：运行确认。

#### P0-7：构建兼容身份对关键配置发生碰撞

1. **位置**：`CMakeLists.txt:231-246`，
   `cmake/ComputeBuildIdentity.cmake:83-96,176`，
   `tests/test_build_identity.cc:61-116`。
2. **行为与证据**：`CAPSID_BUILD_COMPILE_FLAGS` 被构造成 CMake list，却作为
   `COMPILE_FLAGS` 单值参数传入；生成记录只剩
   `build_type=<...> lto=<...>`，丢失 ASAN、UBSAN 和 mimalloc。两个全新临时配置
   （普通与 `CAPSID_ENABLE_ASAN=ON`）生成完全相同的记录和 SHA-256
   `853346f7e1a13eb9c1f491c195fd51d17f2f70899a8b3bfe038c2551ee6a8eab`。
   现有测试只要求字段非空并验证自洽 hash，不比较不同配置必须不同。
3. **影响**：可信字节码兼容门不能区分声明上必须隔离的构建，发行证据也无法唯一
   指向关键构建配置。
4. **整改与验收**：用单一规范字符串生成完整 identity，纳入 Capsid 源码修订、编译
   器/目标三元组和所有兼容性选项；matrix test 必须证明每个受控差异改变 ID，完全
   相同的可复现构建保持一致。
5. **状态**：运行确认。

### 12.5 P1 高优先级清单

1. **Runtime 收尾语义**：`capsid_worker_destroy()` 在
   `src/client.cc:1377-1399` 只调用一次 shutdown/flush 后立即关闭 fd，未处理
   `WOULD_BLOCK`，可能丢弃排队 shutdown 和输出尾部。应提供可等待的 graceful close
   状态机，并把强制销毁语义写清。
2. **事件字段完整初始化**：EOF 路径 `src/client.cc:1848-1859` 只写 EXIT type、ID 和
   payload，未清零 flags/status/credit，复用 event 结构时可观察到陈旧字段。
3. **多请求硬超时诊断**：`src/client.cc:1869-1895` 因一个请求超时杀死 worker、清空
   全部请求，却只返回迭代中第一个超时 ID；其他 inflight 请求没有逐项终态理由。
4. **无界运行注册表**：`src/host/managed_host.cc:186-227` 的 operation registry 和
   App mutex table 进程期只增不减。应设置数量/年龄上限或把状态归入有界持久层。
5. **Release CI 证明缺口**：`.github/workflows/testing-validity.yml:87-108` 的 Linux
   Release job 未设置 `CAPSID_BUILD_HOST=ON`；ASAN/UBSAN 也不构建 Host，仅 TSan
   Debug 同时构建 Host 与 worker；macOS Host job又明确关闭 worker。当前没有 Linux
   Release Host+worker 的完整组合门。
6. **框架测试归属盲区**：Hono、itty-router、H3 lifecycle 均通过，但共享 driver
   读取后忽略 LOG/AUDIT；详见第 10.5 节。
7. **平台测试可移植性**：macOS socket artifact 错误码断言和 GNU `strip --strip-all`
   假设应按平台分支；否则本地矩阵不能提供干净信号。
8. **长期运行证据**：尚缺真实 Managed 数据面上的 crash-loop、replacement、SSE、
   slow-client、队列公平性、连续超时和 24h/72h 资源平台化门。

### 12.6 P2 与已排除项

- P2：终态记录按数值最小而非完成最早淘汰；有界存活测试通过，见第 10.1 节。
- P2：C ABI 错误详情、构建/测试诊断和 benchmark 口径仍需工程化，不能用组件微基准
  直接承诺完整 Host 容量。
- 审校报告自身尚未加入 `docs/README.md`，因此当前文档索引测试失败；这是交接物集成
  状态，不是 Runtime 功能缺陷。正式接纳本报告时应一并加入索引。
- 已排除：本轮 1536 次取消未出现持续 Promise heap 增长，见第 10.2 节。
- 已排除：未知 IPC event 静默忽略、spawn 必须同步 READY 这两项旧判断。

### 12.7 整改顺序与最终发布验收

建议严格按依赖顺序整改：

1. BigInt 请求 ID、RequestToken、终态门禁和 worker replacement。
2. Managed listener/routing、可信键、generation 原子切换与 drain。
3. C ABI 异常封口、Runtime graceful close 和完整终态表达。
4. build identity、install、CPack 与最终包 smoke test。
5. Linux Release 同时启用 Host+worker+WPT；补齐 ASAN、UBSAN、TSan 和 delegated
   strict sandbox 组合。
6. 执行 Managed 数据面故障注入、容量、公平性、恢复和长稳验收。

最终发布门必须同时满足：全部 P0 关闭；P1 中 Runtime 收尾和 Release CI 关闭；全新
Linux Release 构建的完整测试矩阵通过；最终 Capsid 压缩包从空目录安装后可启动
Host、加载 worker、通过真实 HTTP 请求、完成 graceful shutdown；发布包清单、构建
身份和 CI 证据能唯一关联到同一提交与配置。

面向实施者的接口、状态机、工作包、提交拆分和逐项测试门已经进一步冻结在
`docs/capsid-remediation-execution-spec-2026-08-09.md`。该文档是 DeepSeek 的直接执行
入口；本报告继续作为缺陷证据和优先级来源。
