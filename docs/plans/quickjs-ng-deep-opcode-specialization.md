# quickjs-ng 深度 Opcode 专门化与扩展技术方案

> 执行者：其他 AI。本文件是自包含的研究、实现与裁决方案。执行前先确认
> Capsid 基线至少包含 P16 keep 裁决（当前文档基线：`d32378a`，P16 实现：
> `40c3d8a`），并通读 `docs/bytecode-aot-optimizer.md` §3、§5、§11。
> 本方案的正确性门禁以 **quickjs-ng 自身测试和 test262** 为主；Capsid
> 测试只验证最终 overlay/bytecode 集成，不代替引擎语义证明。

## 0. 结论与预期

当前 quickjs-ng 的最终字节码使用单字节 opcode：序列化 opcode 为 0..184，
short opcode 为 185..251，`OP_COUNT = 252`，252..255 尚未使用。直接为每种
专门化分配一个字节只能容纳四种新操作，而且用完后失去非法字节哨兵，不适合
承载长期的 arithmetic/property/call/fusion 组合设计。

本方案采用混合布局：

```text
0..251   既有 opcode，编号和语义不变
252      OP_ext：扩展 opcode 前缀，后跟 u8 ext_id 与对应 payload
253..254 direct slot：只授予实测证明对二级 dispatch 敏感的廉价热操作
255      永久非法，reader/verifier/VM fail closed
```

格式升级为 **BC_VERSION 27**。旧 VM 必须在反序列化阶段拒绝 BC27；新 VM
也不隐式读取 BC26。Capsid 的 trusted-bytecode compatibility ID 和源码回退
负责跨版本部署，不在 QuickJS reader 中维护双版本兼容层。

收益目标按“广谱 workload 几何均值”定义，而不是挑选单个 microbench：

| 实现深度 | 广谱合理区间 | 稳定热点 workload | 解释 |
| --- | ---: | ---: | --- |
| 仅 superinstruction | 3%~10% | 10%~20% | 主要节省 interpreter dispatch |
| 自适应类型/属性/调用专门化 | 8%~20% | 20%~40% | 同时绕开通用查找与慢路径分派 |
| 专门化 + 融合 + 完整 quickening | 15%~25% | 个别内核 40%~60% | 超过该区间通常需要执行表示或 JIT 级改造 |

首轮正式 keep 目标取保守值：**全性能语料几何均值 ≥2%，至少两个目标 lane
≥5%，确认后任何 lane 不得回退 >2%，峰值 RSS 增量 ≤1%**。预计首轮最终
可留下 8%~15% 的组合收益；15%~25% 是多轮覆盖后的目标，不是预先承诺。

## 1. 上游历史与本轮边界

quickjs-ng 曾在 commit `6b78c7f`（PR #120）实现多态 property inline
cache，在 `c7ca3fe`（PR #334）修正 IC opcode 不应直接序列化，最后在
`7de6d46`（PR #884）删除整套 IC。删除原因是性能分化和内存增长：上游
web-tooling 数据几何均值约 +3.67%，但 prettier/babel-minify 显著提升，
typescript、babel、babylon 等又回退。参考：

- <https://github.com/quickjs-ng/quickjs/issues/876>
- <https://github.com/quickjs-ng/quickjs/pull/120>
- <https://github.com/quickjs-ng/quickjs/pull/334>
- <https://github.com/quickjs-ng/quickjs/pull/884>

本轮**不直接恢复旧 IC**。旧提交只用于核对以下成熟经验：shape guard、
property offset、GC mark/free、运行时 opcode 改写和 serializer canonicalize。
新实现必须解决旧方案的两个核心缺陷：冷函数也承担 IC 状态，以及按 atom
预建 hash/ring 导致收益小的站点同样占用内存。

本方案做：

1. quickjs-ng 原生 opcode 动态 profiling；
2. arithmetic、object access、call、superinstruction 四类统一排序；
3. BC27 `OP_ext` 基础设施；
4. 排名前两位候选的独立原型、A/B 和组合裁决；
5. 仅将通过门槛的实现接入 Capsid overlay。

本方案不做：

- 不预先指定 property IC 一定获胜；
- 不引入 JIT、baseline compiler、NaN-boxing 重构或 16-bit decoded IR；
- 不以 Capsid/Hono 请求替代 quickjs-ng 自身语料；
- 不改变 ECMAScript 可观察语义、异常顺序、GC root 或调试行号；
- 不因为实验结果好看而放宽 test262 或单项回退门槛。

## 2. 当前代码锚点

以下锚点基于 vendored quickjs-ng commit `bf8988f`；执行时若 vendor 已升级，
必须用符号/模式重新定位，禁止照抄行号：

| 位置 | 当前职责 |
| --- | --- |
| `quickjs-opcode.h` | `DEF/FMT` 表；`nop` 后为 temporary，再后为 short opcode |
| `quickjs.c:1150-1185` | `OPCodeFormat`、`OPCodeEnum`、`OP_COUNT`、temp overlap |
| `quickjs.c:779-814` | `JSFunctionBytecode`；bitfield 尚有 5 bit 可用 |
| `quickjs.c:17595-17620` | direct-threaded `dispatch_table[256]` 与 default range |
| `quickjs.c:21856` 附近 | `JSOpCode opcode_info` 与 `short_opcode_info` |
| `quickjs.c:32367-32435` | bytecode dump/decode 的 opcode 合法性与长度读取 |
| `quickjs.c:35907-36101` | `compute_stack_size` verifier |
| `quickjs.c:37686` | `BC_VERSION 26` |
| `quickjs.c:37840-37905` | `JS_WriteFunctionBytecode` atom 重写 |
| `quickjs.c:39689` 附近 | reader 的 BC_VERSION gate |
| `Makefile:96-141` | jscheck、test、test262、test262-fast、microbench |

Capsid 集成锚点：

| 位置 | 后续工作 |
| --- | --- |
| `patches/txiki/` | 所有 quickjs-ng 改动必须形成 overlay patch，不直接改 vendor |
| `cmake/ComputeTxikiOverlayKey.cmake` | patch 数量由 36 更新为最终数量 |
| `cmake/AuditTxikiVendor.cmake` | patch inventory、说明与数量同步 |
| `docs/txiki-upgrade-baseline.json` | overlay key/manifest 更新，QuickJS gitlink不变 |
| `tools/bytecode_optimize.cc` | BC27、扩展表、decode/verify/emit/P7 适配 |
| `CMakeLists.txt` | `bytecodeFormatIdentity` 从 v1 升级为明确的新 identity |

## 3. Phase A：quickjs-ng 原生 Opcode Profiling

### 3.1 编译开关与接口

新增 `CONFIG_OPCODE_PROFILE`，只在 profiling build 定义。未定义时不得在
`JSRuntime`、`JSFunctionBytecode` 或 dispatch handler 中留下字段、计数分支
或链接符号，先用生成代码/`sizeof` 对照证明 release build 零开销。

在 `quickjs.h` 的同一编译门控下增加：

```c
#ifdef CONFIG_OPCODE_PROFILE
JS_EXTERN void JS_DumpOpcodeProfile(FILE *fp, JSRuntime *rt);
#endif
```

`qjs` profiling build 增加 `--opcode-profile FILE`。程序正常结束时把一个
JSON 对象写到 FILE；打不开文件、短写或 JSON 生成失败必须令 qjs 非零退出，
不能静默丢证据。普通构建不接受该参数。

固定 JSON schema `quickjs-ng-opcode-profile-v1`：

```json
{
  "schema": "quickjs-ng-opcode-profile-v1",
  "quickjsCommit": "40-hex",
  "build": { "compiler": "...", "arch": "...", "nanBoxing": true },
  "totals": { "executions": 0, "sampledTicks": 0 },
  "opcodes": [
    {
      "id": 0,
      "name": "add",
      "executions": 0,
      "samples": 0,
      "sampledTicks": 0,
      "slowPath": 0,
      "classes": { "i32_i32": 0, "number_number": 0, "other": 0 }
    }
  ],
  "transitions": [{ "ops": [0, 0, 0], "count": 0 }],
  "sites": [{ "function": 0, "pc": 0, "op": 0, "hits": 0,
               "stable": 0, "polymorphism": 0 }]
}
```

profile 文件不得写地址、源码路径或 atom 文本；function 使用本次运行内按
解析顺序分配的稳定序号，避免 ASLR 和敏感源码进入 artifact。

### 3.2 计数内容

- 所有 opcode：动态次数、前驱 opcode、二元组和三元组；transition 表只在
  profile build 存在，最终 JSON 仅输出 top 256。
- 采样成本：每 1024 次同 opcode 执行抽样一次 handler elapsed ticks；x86_64
  使用有序 cycle counter，其他平台使用单调高分辨率时钟。结果只用于同机
  排序，不跨架构比较绝对 ticks。
- arithmetic/compare：`i32+i32`、number+number、string、BigInt、other，及
  slow helper 次数。
- named/global/index access：object tag、own normal-data property、prototype
  hit、accessor、proxy/exotic、shape 稳定率、dense array/typed array/hole。
- call：JS bytecode、C function、bound/proxy、argc 0..3/other、同站点 callee
  稳定率、exception。
- control/fusion：taken/not-taken、backedge、序列是否含跳转目标/pc2line 边界。

计数只能旁路观察；禁止为了方便 profiling 改变 fast/slow path 分支顺序。

### 3.3 语料与产物

正确性测试不用于性能加权。性能采集使用：

1. quickjs-ng 自带 `tests/microbench.js`，每个 lane 单独运行；
2. quickjs-ng 上游用于 #876/#884 的 web-tooling benchmark，固定 revision；
3. 为四类候选新增最小 microbench，但它们只测 ceiling，不进入广谱几何均值。

每项 1 次预热 + 7 次测量，固定 CPU、governor 和构建产物。保存到
`bench/results/quickjs-opcode-profile-<timestamp>/`：manifest、原始 JSON、
perf stat、wall-time 样本、峰值 RSS、二进制 sha256。不要提交大体积 perf.data。

### 3.4 自动排序

候选分数：

```text
score = dynamic_executions × sampled_cost_per_execution × specializable_hit_rate
```

四类各生成候选，然后全局排序：

- 类型专门化：实际最热的 arithmetic/compare 操作及其主导类型；
- 对象访问：named property、global 或 array/index 中实际最热且稳定者；
- 调用：稳定 JS callee/method/fixed-argc；
- 融合：top transition 中无内部 target、异常/栈语义可原样合并的序列。

候选进入原型池的硬条件：`specializable_hit_rate ≥ 90%` 且估计覆盖总
interpreter sampled ticks ≥2%。选 score 前两名；score 相同则依次选择
per-site 内存更少、语义面更小、升级冲突更低者。若不足两个，只实现实际
达标数量；若一个都没有，停止 Phase B/C，提交 profiling 与 no-go 报告。

## 4. Phase B：BC27 与混合 Opcode 空间

### 4.1 表结构

新增 `quickjs-ext-opcode.h`，由同一个 `EXTDEF(id,size,pop,push,fmt)` 表生成：

- `ExtOpcodeEnum`，`EXT_invalid = 0`；
- `ExtOpInfo`；
- dump 名称；
- direct/secondary dispatch labels；
- serializer/byte-swap operand 分类。

主表在全部既有 short opcode 后追加 `DEF(ext, 2, 0, 0, ext)`，因此
`OP_ext == 252`。这里的 size/pop/push 只是前缀占位；任何 decode、verify、
emit 或 dump 看到 `OP_ext` 后都必须读取 `ext_id` 并改用 `ExtOpInfo`，禁止
继续使用主表的占位栈效果。

实现阶段的计数规则：

- 仅 `OP_ext` 时 `OP_COUNT == 253`，253..255 进入 `case_default`；
- 第一个 direct winner 追加后 `OP_COUNT == 254`；
- 第二个 direct winner 追加后 `OP_COUNT == 255`；
- 255 始终落入 default，永不定义。

增加静态断言：既有 `OP_get_field`、`OP_nop`、首个/末个 short opcode 编号
保持当前值；`OP_ext == 252`；`OP_COUNT <= 255`；所有 ext total size 在
1..255 且 payload 与 format 相符。

### 4.2 编码与跳转

通用扩展编码：

```text
u8  OP_ext (=252)
u8  ext_id (1..255)
... payload defined by ExtOpInfo
```

`ExtOpInfo.size` 是包含 prefix+ext_id 的总长度。需要 atom/slot/site/label 的
候选分别定义显式 format，不用未对齐 struct memcpy。所有 u16/u32 和 label
沿用 quickjs-ng 当前小端 helper；byte-swap、atom index rewrite、label update
与 dump 都由 format 驱动。

扩展 branch/superinstruction 的 label 仍以 operand start 为相对基准，并在
表注释中写明；`resolve_labels`、`compute_stack_size`、serializer 和 Capsid
optimizer 必须共享这一规则。任何 target 落在扩展指令 payload 中立即失败。

### 4.3 VM dispatch

direct-threaded 构建的 `CASE(OP_ext)` 读取 `ext_id` 后，通过 256 项
`ext_dispatch_table` 做 secondary computed-goto；非 direct-threaded 构建用
switch。ext_id 0、未定义项、截断 payload 均进入 `case_default`。

`OP_ext` 不允许递归扩展，不允许 ext handler 自行把 pc 指向 payload 中间。
cheap opcode 是否晋升 253/254 只能由 Phase C 的 direct-vs-ext A/B 决定。

### 4.4 BC_VERSION 与 canonicalization

- `BC_VERSION` 从 26 改为 27；checksum 算法不变。
- runtime quickening 只允许改写 `ext_id` 为同 size/format 的状态变体，payload
  中的原 atom/operand 保持不变。
- writer 遇到 mono/poly/mega 等运行时状态时，一律 canonicalize 回对应
  adaptive ext_id，再做 atom index 和端序处理；不得序列化 shape、pointer、
  hit counter 或 cache offset。
- reader 只接受 canonical ext_id；runtime-only 状态出现在 bundle 中必须
  fail closed，而不是默默降级。
- 相同源代码、相同构建和相同选项生成的未执行 bundle 必须 bit-for-bit
  deterministic；运行后重新序列化也必须回到同一个 canonical bundle。

## 5. Phase C：热点状态与两个候选原型

### 5.1 冷代码零分配

利用 `JSFunctionBytecode` 当前 bitfield 的 5 个空闲 bit 增加运行时
`quicken_score : 5`，不写入 function flags，不改变序列化布局，并用
`sizeof(JSFunctionBytecode)` before/after 静态/运行测试确认 struct 不增长。

score 饱和于 31：函数入口 +1，taken backedge +1。实验阈值固定为 4、12、
24；达到阈值后扫描该函数的 ext site 并在 `JSRuntime` 的 hot-function side
table 中分配状态。冷函数没有 heap allocation 或 shape root。阈值选择规则：
满足所有回退/RSS 门槛后几何均值最高者；相同时选择更高阈值。

side table 以 `JSFunctionBytecode*` 为 key，但 profile/JSON 不输出地址。函数
bytecode 销毁时必须移除状态；其中持有的 GC 对象/shape 必须参与 mark/free。
站点默认最多保存两个 observed variants；第三种 shape/type/callee 或连续 8 次
miss 后标记 megamorphic，本次函数生命周期不再反复 specialize/deopt。

### 5.2 候选模板

Phase A 的前两名分别套用下列模板，不由执行者主观换题：

- **numeric/compare**：guard 主导 tag；fast path 必须复用 generic handler
  的溢出、`-0`、NaN 和异常规则；miss 调同一个 slow helper。
- **named/global access**：只缓存经证明安全的 own normal-data property；guard
  object tag + exact shape + property offset/flags；prototype、accessor、Proxy、
  exotic 和 primitive receiver 全走原路径。
- **array/index**：guard exact class、fast-array/typed-array 状态、integer index、
  bounds、hole/detach；任何 guard miss 回原 `get_array_el`/对应 handler。
- **call**：guard callee identity/type、realm、argc、receiver/constructor mode；
  async/generator/native/proxy 不在首版 fast path 时必须回原 `JS_Call`。
- **superinstruction**：只融合动态 top 序列；中间不得是 jump target、catch/
  gosub root 或独立 pc2line entry；合并后 pop/push、释放顺序、interrupt poll、
  `sf->cur_pc` 和异常位置必须与原序列一致。

每个原型提供编译时 `OFF`、`ALWAYS`、`ADAPTIVE` 三态。`ALWAYS` 只测理论
ceiling，不能直接作为 keep 配置；正式候选必须以 `ADAPTIVE` 通过门槛。

### 5.3 direct slot 晋升

所有候选先作为 `OP_ext` 子操作测量。只有同时满足以下条件才复制为 253 或
254 的 direct opcode：

1. ext secondary dispatch 占该 handler 总成本 ≥10%；
2. direct 相对 ext 在目标 lane 再提升 ≥3%；
3. 该候选覆盖总 interpreter sampled ticks ≥5%；
4. direct 与 ext 的语义、payload 和测试完全同源，不能维护两份逻辑。

未达到条件的候选永久留在 ext 空间；未使用的 direct slot 保持非法，不为了
“用满预算”分配。

## 6. 正确性主门禁：quickjs-ng 自身测试 + test262

本节优先级高于性能测试。每个阶段先运行 quickjs-ng 原生门禁；任何失败都先
修复，禁止用 Capsid differential 或 benchmark 输出替代。

### 6.1 每次改动的快速门禁

在应用新 patch 的独立 quickjs-ng/txiki overlay 构建树中运行：

```sh
make jscheck
make ctest
make cxxtest
make
make test
./build/api-test
./build/lre-test
./build/qjs tests/test_bjson.js
make test262-fast
```

若 test262 子模块未初始化，先在 vendored quickjs gitlink 内执行：

```sh
git submodule update --init --checkout --depth 1 test262
```

`make test` 使用 quickjs-ng 自身 `tests.conf`；`test262-fast` 使用
`test262.conf + test262-fast.conf`。两者都必须 0 unexpected failure；不能只
比较总通过数。

### 6.2 候选 keep 前的完整门禁

每个候选独立和最终组合都必须运行：

```sh
make test262
make test262-check
```

同时复现 quickjs-ng CI 的以下构建/测试形态：

- Debug 与 Release；
- GCC 与 Clang；
- `JS_NAN_BOXING=0/1` 的 ctest/cxxtest；
- parserless build + generated standalone bytecode；
- Linux ASan+UBSan；可用环境再跑 MSan、TSan 和 Valgrind test262-fast；
- Linux、macOS、Windows 至少各一个原生 CI 构建，验证 switch/direct dispatch。

完整 test262 结果、config revision 和 expected-error 基线一起归档。新增失败
必须给出具体 test262 路径和语义归因；不得修改 `test262_errors.txt` 掩盖回归。

### 6.3 新增 quickjs-ng 定向测试

在 quickjs-ng 自身 `tests/`/`api-test.c` 增加：

1. BC26 被 BC27 VM 明确拒绝；BC27 被旧 VM 明确拒绝；错误中包含版本；
2. ext_id 0、未定义 ext_id、截断/超长 payload、payload 内 jump target；
3. canonical write→read→execute，以及执行/quickening 后再次 write 的字节一致；
4. atom、u16/u32、label 和 variable-pop ext format 的 round-trip；
5. direct-dispatch 与 switch-dispatch 输出一致；
6. 每个候选的 fast hit、guard miss、mono→poly→mega、exception、GC；
7. property 的 getter/setter/Proxy/prototype/shape mutation/delete/redefine；
8. numeric 的溢出、double、NaN、Infinity、`-0`、string、Symbol、BigInt；
9. array 的 hole、越界、length 变化、typed-array detach；
10. call 的 callee 替换、native/bytecode/bound/proxy、constructor、async/
    generator、realm；
11. fusion 的每个跳转边界、catch/finally、interrupt、pc2line 与异常顺序；
12. runtime side table 在函数/realm/runtime 释放后无 leak、UAF 或残留 GC root。

## 7. 性能与资源裁决

性能测试只在 §6 全绿后运行。每个状态 1 warmup + 7 measured rounds，报告中位
数、几何均值、MAD、cycles、instructions、IPC、branches、branch-misses、
task-clock、peak RSS 和 quickening metadata 字节数。

状态矩阵：

```text
baseline BC27/ext infrastructure, no candidate emitted
candidate A: OFF / ALWAYS / ADAPTIVE(threshold 4/12/24)
candidate B: OFF / ALWAYS / ADAPTIVE(threshold 4/12/24)
A+B selected adaptive thresholds
ext winner vs direct form（仅满足 §5.3 时）
```

基础设施自身必须满足：无 ext opcode 的 microbench 相对原 BC26 build 回退
<1%，峰值 RSS 和 `sizeof(JSFunctionBytecode)` 不增长。候选 keep 条件：

1. 至少两个目标 lane median 提升 ≥5%；
2. 全性能语料几何均值提升 ≥2%；
3. 任一 >2% 回退必须交错复测；确认后即 trim；
4. peak RSS 增量 ≤1%；hot-site metadata 单站点上界有报告；
5. profile 证明收益来自候选 fast path，命中率仍 ≥90%；
6. A、B 各自过门后组合重测；组合不过则只保留单项收益更高者。

若无候选通过：不发射 ext opcode，不升级 Capsid 生产格式；profiling patch 可在
证明 release 零开销后保留，BC27/ext patch 不进入生产 overlay，并提交 no-go
报告。不能因已实现 BC27 而降低 keep 门槛。

## 8. Capsid Overlay 与 AOT Optimizer 接入

只在至少一个候选通过 §6/§7 后执行。建议拆为两个 upgrade-friendly patch：

- `0036-quickjs-opcode-profile.patch`：profile 开关与工具；
- `0037-quickjs-ext-opcodes.patch`：BC27、OP_ext、通过裁决的候选。

如果 profiling 不进入生产，可只交付最终 opcode patch，并按实际 patch 数更新
两个 CMake inventory；不要保留空 patch。开发时在 pinned QuickJS commit 的
临时 worktree 实现/测试，再生成 `deps/quickjs/...` 路径的 txiki overlay
patch，禁止直接修改 `vendor/txiki.js`。

Capsid optimizer 必须在编译器开始输出 BC27 前完成：

- header gate 26→27，仍校验 checksum；
- 从同一 ext table 生成/同步 `ExtOpInfo`，拒绝未知 ext_id；
- decode、stack verifier、jump target、emitter、P7 pc2line 支持 ext size/format；
- ext 候选若读写 local slot，补入 P2/P11/P14/P16 精确分类；否则按 barrier
  处理，不能让现有 pass 穿过未知副作用；
- byte-identical no-trigger fixture 和 malformed/fuzz corpus 增加 BC27/ext cases。

同步更新：overlay patch count/说明、`docs/txiki-upgrade-baseline.json` 的 key/
manifest、`CMakeLists.txt` 的 bytecode format identity、build identity matrix、
architecture/optimizer/performance 文档。兼容性测试必须证明新旧 compiler、
worker、Host identity 任一错配都会拒绝 trusted bytecode 并按既有规则回退源码。

## 9. 实施顺序与提交边界

1. **A0 基线**：固定 quickjs commit、编译器/CPU、原生测试结果、test262-fast/
   full test262 结果、microbench 与 RSS；只归档，不改 VM。
2. **A1 profiling**：`CONFIG_OPCODE_PROFILE`、JSON API/CLI、定向自测；跑 §6.1
   和 test262-fast 后采集完整 profile。
3. **A2 选择**：生成四类矩阵，按 §3.4 自动选前两项；提交 profile 报告和
   候选名称/命中率/覆盖率，之后不再换候选。
4. **B1 格式**：BC27、OP_ext、ExtOpInfo、reader/writer/dump/verifier/dispatch；
   先不发射候选，跑完整 §6。
5. **C1/C2 原型**：候选 A、B 各自独立实现和提交，逐个跑 §6.1、定向测试、
   test262-fast；二者都完成后跑 full test262。
6. **C3 裁决**：OFF/ALWAYS/ADAPTIVE、组合、direct 晋升矩阵；keep/trim 写死。
7. **D1 overlay**：只携带保留代码生成 patches，更新 Capsid optimizer 与
   identity；运行 QuickJS 完整门禁后再跑 Capsid bytecode/differential/fuzz。
8. **D2 收尾**：归档原始证据、更新文档、记录未通过候选和 opcode/direct
   slot 最终占用，不删除负结果。

每个阶段单独 commit，禁止把 profiling、格式、两个候选和裁决混成一个不可
归因提交。最终交付必须列出：QuickJS 自身测试、test262-fast、full test262、
sanitizer、性能矩阵、RSS、opcode 分配、BC27 identity 和 Capsid 回退证明。

## 10. 风险与强制停止条件

| 风险 | 处理 |
| --- | --- |
| ext 二级 dispatch 吃掉廉价操作收益 | 先 ext A/B；仅满足 §5.3 才用 direct slot |
| `OP_COUNT` 到 255 后 default range/比较溢出 | 静态断言 + switch/direct 两种构建；255 永久非法 |
| runtime 改写污染序列化 | 只改同 size ext_id；writer canonicalize；round-trip 字节测试 |
| cold function 内存/启动回退 | 5-bit inline score + hot-only side table；基础设施 <1% gate |
| shape/callee cache 变成悬挂指针 | GC mark/free + 函数销毁移除 + ASan/Valgrind/GC 压力测试 |
| superinstruction 改变异常/行号 | target/pc2line/catch root 禁区 + 原生定向测试 + test262 |
| Capsid AOT 错解 ext 指令 | 编译器发射前先完成 optimizer BC27/ext 支持；未知形态 fail closed |
| benchmark 好但语言语义回归 | quickjs tests + test262-fast/full test262 是硬门禁，性能不得豁免 |
| 两个候选都未达门槛 | 停止生产接入，保留证据；不因 sunk cost 发布 BC27 |

