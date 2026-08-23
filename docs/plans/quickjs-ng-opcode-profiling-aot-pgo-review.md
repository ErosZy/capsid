# quickjs-ng Opcode Profiling/AOT-PGO 方案技术评审与修订建议

> 评审对象：`docs/plans/quickjs-ng-opcode-profiling-aot-pgo.md`。
> 本文是给方案作者和后续执行 AI 的自包含修订输入，不替代原方案。
> 结论基于 Capsid 当前实现、固定的 quickjs-ng vendor 源码，以及
> `/home/eroszhao/sablejs/docs/optimization-plan.md` 和 `performance.md` 中
> 已记录的 AOT 实践。

## 0. 执行结论

原方案的总体方向成立，值得保留的核心是：先 profile 再选候选、AOT 静态
证明优先于运行时 quickening、负结果归档、配对交错 A/B，以及以 quickjs-ng
自身测试和 test262 作为正确性主门禁。这些原则与 sablejs 已验证的方法论一致，
也直接回应了 quickjs-ng 上游 IC 在提交 `7de6d467` 中因性能分化、内存增长而
被删除的问题。

但当前文本还不能直接交给实现 AI 开始 BC27/P17/P18。以下六项是实现前
阻塞项：

1. `call/call_method` 不携带 function atom，现有过程间分析前提不成立；
2. P9 SSI 实现已经删除，P17 不是现存 SSI 的免费扩展，且事实源表含不存在的
   opcode；
3. Lane 2 的 exact-shape/exact-callee guard 与“无任何运行时状态”不能同时成立；
4. profile 的 `(function atom + pc)` 不能作为跨构建的唯一 site identity；
5. A2 在 P17 之前无法给出精确的静态可证明份额，需要独立 analyze-only 阶段；
6. 普通 `make test262` 不经过 Capsid AOT 发射路径，无法验证新 opcode。

建议状态：**A0 可以立即执行；A1 在 schema 修正后执行；A2/A3 完成 go/no-go
前禁止启动 BC27/OP_ext。** 如果没有候选同时通过动态成本门和静态/guard
可实现性门，提交 profiling 与 no-go 证据，不升级格式。

## 1. 对 sablejs 经验的采纳边界

原方案正确采纳了以下经验，应原样保留：

- validation-first：先证明目标动态份额，再建设 pass；
- try-measure-reject：覆盖接近零或实测回退的实现必须删除并归档；
- in-session interleaved A/B：跨会话绝对值不能当作改动证据；
- 首测异常必须扩大轮数复检；
- 每条优化规则提供 soundness 论证，并把发现的具体 bug 固化为回归测试；
- kill switch、逐候选统计和真实部署 workload 地板是 keep 的必要条件。

需要收窄的是量化类比。sablejs 把算术降为原生 JavaScript 运算符后，后续由
V8 JIT 完成寄存器分配、类型反馈和机器码优化；quickjs-ng 新增专门化 opcode
后仍然执行 boxed `JSValue`、解释器 dispatch 和引用计数。二者在结构上相似，
但不是同一级别的“原生化”。因此 sablejs 的 `+45%` local promotion 或整套
收益只能支持本方案的方法论，不能作为 quickjs-ng 的收益先验。

## 2. 阻塞修正一：调用点与过程间分析

### 2.1 as-built 事实

固定 vendor 的 `quickjs-opcode.h` 定义为：

```text
DEF(call,             3, 1, 1, npop)
DEF(call_method,      3, 2, 1, npop)
DEF(call_constructor, 3, 2, 1, npop)
```

三者的 `u16` 操作数是 `argc`。解释器在 `OP_call` 中从操作数栈取得
`call_argv[-1]` 作为 callee，在 `OP_call_method` 中取得栈上的 callee 和
receiver。它们没有 function atom、function slot 或 child-function index。

只有 `fclosure/fclosure8` 自身携带 cpool index。要从调用点定位 callee，必须
证明该栈值来自某个 `fclosure`，并在经过 `put_loc/get_loc`、分支 join、闭包捕获、
属性存取和调用屏障后仍保持唯一身份。

### 2.2 修订后的首轮边界

首轮 P17 不做一般过程间返回类型传播。把调用结果一律设为 `T_UNKNOWN`，先验证
函数内静态数值证明是否有足够动态覆盖。

只有 A2 显示 call/return 事实会显著增加已选候选覆盖时，才增加独立的 closure
identity lattice：

```text
F_EXACT(function_cpool_path)
F_UNKNOWN
```

进入 `F_EXACT` 至少要求：

- 生产者是当前 bundle 中可定位的 `fclosure/fclosure8`；
- 所有传播路径保持同一 function cpool path；
- 不经过属性/global/var_ref/未知调用返回；
- closure 未逃逸到 host、export、动态属性或未知容器；
- callee 存在未知调用方时，参数类型和环境依赖事实全部为 unknown；
- eval、with、动态 import、host callback 和无法证明的递归 SCC 直接降级。

“bundle 中能读取所有 function bytecode”不等于 closed world。导出入口、host
回调、间接属性调用和动态代码仍能产生 bundle 外调用方，不能据此给参数或
receiver 注入精确类型。

## 3. 阻塞修正二：P17 的真实建设量与最小类型域

### 3.1 现有资产的准确描述

当前可直接复用的是：

- P2 的槽值格、captured/eval/with 屏障思想；
- P14 的 cpool tag/value 读取器；
- P16 的槽编号、读写分类和后向活性模型；
- verifier 的精确 stack height；
- `FuncRecord.children` 的序列化 function cpool tree。

P9-P15' SSI 实现已经由提交 `8078d04` 删除。文档可以作为设计参考，但源码中
不存在可扩展的 φ/SSI layer。现有 P2 只跨 BB 保存 slot lattice；operand stack
类型在每个块内最多保存 top/prev，不能直接支持任意 join 后的双操作数证明。

### 3.2 事实源纠正

serialized opcode 中存在 `push_i16`，不存在 `push_16` 和 `push_f64`。F64
常量要通过 `push_const/push_const8` 对应的 cpool tag 识别。对象有 `OP_object`；
数组字面量通常是一段构建序列，不能假定存在单一 `array` producer opcode。

### 3.3 candidate-specific analyze-only 格

不要在候选未确定前建设完整 `T_INT/T_F64/T_STR/T_OBJ/T_ARR` 格。A2 选出
候选后，A3 只建设支持候选的最小证明域。例如候选是 `add_i32i32` 时：

```text
BOTTOM       不可达/尚无前驱
EXACT_INT    运行时必为 JS_TAG_INT
UNKNOWN      任意 JSValue
```

实现必须维护每个 basic block 的完整 operand-stack 类型向量和 slot 类型向量，
并与 verifier 的 stack height 完全一致。join 规则是：

```text
join(BOTTOM, x)        = x
join(EXACT_INT, INT)   = EXACT_INT
join(any other pair)   = UNKNOWN
```

每个 serialized opcode 都必须有 transfer 分类；未分类 opcode 默认把受影响栈值
和可见槽降为 unknown/barrier，而不是继续传播。

### 3.4 异常路径

当前 CFG 不为每条可能抛异常的指令建立 exception edge。P17 首版必须二选一：

1. 含 `catch/gosub`、try/finally 或动态环境的整个 function 不发射；或
2. catch/finally handler 的入口 stack/slot 状态强制为 unknown，并证明任何
   exceptional entry 都不会被普通前驱的精确状态覆盖。

在有独立异常 CFG 之前，方案 1 更稳健，也符合 sablejs DSE 在发现
try/finally soundness bug 后整 scope 跳过的先例。

## 4. 阻塞修正三：Lane 2 拆分

### 4.1 为什么 numeric static guard 可能没有收益

quickjs-ng 的 `OP_add/sub/mul/compare` 已经把 int-int 放在第一个 likely fast
path；`OP_add/sub/mul` 还包含 float-float fast path。一个带同样 tag guard 的
新 opcode 只改变分支布局，并未消除 tag check。若使用 `OP_ext`，额外的二级
dispatch 可能大于节省。

因此 numeric Lane 2 不能因 `i32_i32 stable >=90%` 自动进入原型。必须先证明
它相对 generic handler 真正少执行了什么，并做 patchless/OFF/ON 三态 A/B。
float-first 重新排序、跨多条指令共享一次 guard 或融合 dispatch 才是较可信的
无状态候选。

### 4.2 为什么 exact shape/callee 需要状态

named-property 快路径需要比较当前对象的 runtime `JSShape*` 与期望 shape；
profile 中的地址不能跨进程写入 bundle。callee closure identity 同样是运行时
对象。若每次根据 atom/property layout 重新结构化验证，成本可能接近通用查找，
也不能等价替代 exact identity。

所以应拆成两类：

| Lane | 允许的候选 | 运行时状态 |
| --- | --- | --- |
| 2A：无状态静态 guard/fusion | tag、class、argc、可序列化 cpool identity、融合序列 | 无 |
| 2B：PGO-seeded sparse cache | exact shape、property offset、callee identity | 仅 profile 热 site |

Lane 2B 的价值不是声称“没有 IC”，而是把上游全量 IC 改为 **PGO 筛选后的稀疏
IC**：冷 site 没有字段、root 或分配；热 site 的状态上限、GC 标记、失效和
megamorphic 规则仍必须完整实现和测试。

如果产品红线仍是绝对零运行时状态，则 named-property/callee 候选必须从 Lane 2
删除，只保留 2A，并接受收益上限明显下降。

## 5. 阻塞修正四：PGO site identity 与产物契约

`function atom + pc` 不能唯一标识 site：匿名/同名函数会碰撞，atom 不是 function
identity，运行内序号也未必能跨 bundle 装载顺序重现。

profile v1 的 site key 应固定为：

```text
prePgoBundleSha256
moduleOrdinal
functionCpoolPath[]
originalPc
originalOpcode
```

其中 `functionCpoolPath` 是从根 function 开始、按 cpool 序列化顺序进入 child
function 的 index path，不含地址、源码路径或 atom 文本。

采集和消费必须使用同一个 canonical pre-PGO bundle：

```text
source
  -> 当前 kPassAll，输出 canonical BC26 bundle B
  -> profiling worker 执行 B，profile 绑定 sha256(B)

source + profile
  -> 重新生成 B
  -> bit-for-bit 校验 sha256(B)
  -> 以 functionCpoolPath + originalPc 匹配 site
  -> 才运行 P17/P18/PGO 发射
```

任何 digest、build identity、opcode 或 function path 不匹配都 fail closed。这样
P11/P14/P16/P2 删除和改写指令后，不会把旧 profile 错配到新的 PC。

`stable >=90%` 还必须绑定最小 hit 数和置信度。建议使用 Wilson lower bound
`>=0.90`，并预登记最小 hit 数；不能让 `9/10` 与 `900000/1000000` 等价。

## 6. 阻塞修正五：增加 A3 analyze-only 裁决

原顺序要求 A2 报告“Lane 1 静态可证明份额”，但 P17 到 C1 才实现。修订为：

1. **A0 baseline**：固定 vendor/build/CPU、原生测试、test262、性能和 overlay
   冲突地图；
2. **A1 profiling**：只加入 profile build，不改生产 VM/BC_VERSION；
3. **A2 dynamic ranking**：按 sampled cost、slow-path share、动态次数和稳定率
   选出最多两个候选；
4. **A3 analyze-only proof**：对候选建立最小类型/身份域，只统计可证明动态份额，
   不发射、不改字节码；
5. **A4 go/no-go**：结合动态成本、A3 覆盖和 handler 理论差分，决定是否值得
   新格式；
6. **B1 BC27/OP_ext**：只有 A4 至少一个候选通过才开始；
7. **C1 Lane 1**、**C2 Lane 2A**、**C3 条件 Lane 2B**；
8. **D1 条件 Lane 3**：仅剩余热点仍达标时开始 runtime quickening。

A1 虽然会修改 profiling build 的 VM 源码，但不改变生产 binary、生产 bundle
或字节码 identity；文档中的“不动 VM”应准确表述为“不动生产 VM/格式”。

## 7. 阻塞修正六：以 quickjs-ng/test262 真正覆盖 AOT 路径

### 7.1 quickjs-ng 原生主门禁

每个实现提交至少运行：

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

候选 keep、格式变更或 reader/writer/dispatch 变更前运行：

```sh
make test262
make test262-check
```

不得修改 `test262_errors.txt` 掩盖新增失败。还要覆盖 Debug/Release、GCC/Clang、
`JS_NAN_BOXING=0/1`、computed-goto/switch、standalone/parserless，以及 Linux
ASan+UBSan；可用环境继续跑 TSan、Valgrind 和多平台 CI。

### 7.2 optimized-test262 是独立硬门

普通 `run-test262` 从源码直接进入 quickjs 编译器和 VM，不会自动调用 Capsid
的 serialized-bytecode optimizer。必须增加 test262 AOT adapter：

```text
runtime-positive test262 case
  -> quickjs 编译并序列化
  -> Capsid kPassAll + P17/P18/PGO
  -> BC27 reader
  -> 执行并交回 run-test262 adjudication
```

至少设置以下执行模式：

| 模式 | 目的 |
| --- | --- |
| source baseline | 验证未优化 quickjs 语义和 parse-negative 用例 |
| BC26 serialize/deserialize | 固定旧格式与 round-trip 基线 |
| BC27 generic/no ext | 验证双版本 reader 与空变换 |
| Lane 1 proof emission | 验证真实 P17/P18 发射路径 |
| Lane 2 synthetic-hot | 用合成 profile 强制 guard opcode 发射 |
| guard-hit/guard-miss | 两条路径都必须可达并与 generic 等价 |
| OP_ext/direct | 同一 handler 的两种 dispatch 形态 |
| Lane 3 OFF/ADAPTIVE | 若启用，验证 quickening 前后可观测等价 |

parse-negative 仍由 source baseline 裁决；可序列化的 runtime、module、async 和
异常语义用例进入 optimized 路径。test262-fast 用于每次迭代，所有最终保留
候选至少在 release 配置上跑一次 full optimized-test262。

### 7.3 新格式定向测试

除 test262 外，quickjs-ng 自身测试必须新增：

- BC26/BC27 合法读取与旧 VM 拒绝 BC27；
- ext_id 0、未定义 id、截断 payload、错误 size/format、target 落入 payload；
- atom/slot/label payload 的 endian rewrite；
- runtime-only ext state 禁止从 bundle 读取；
- 执行后重新序列化 canonicalize 回稳定 bundle；
- direct/ext、switch/computed-goto 行为一致；
- overflow、`-0`、NaN、BigInt、Symbol、对象 coercion、getter/Proxy、异常和
  pc2line；
- Lane 2B/Lane 3 若启用时的 GC mark/free、函数销毁、megamorphic 和 OOM。

Capsid RED/round-trip/differential/fuzz 是第二层集成门，不能替代以上 quickjs-ng
原生门禁和 optimized-test262。

## 8. 管线、格式与部署补充

### 8.1 P17/P18 放置位置

首版把 P17 analyze/emission 放在当前 P11/P14/P16/P2/P3.1 固定点结束之后、
P7 pc2line 之前，单次运行：

```text
现有 generic fixpoint
  -> P17 candidate-specific type proof
  -> P18 specialization emission
  -> ext-aware verifier
  -> P7 pc2line
  -> P8 splice/checksum
```

不要在固定点早期把 generic arithmetic 替换成 ext opcode，否则 P2/P3.1 可能
不再识别可折叠指令。只有实验证明 specialized opcode 能继续喂给现有 pass，且
所有 pass 都有 ext transfer/classification 后，才考虑并入固定点。

### 8.2 BC26/BC27 灰度，而不是 flag-day

推荐部署契约：

- 新 VM 同时接受 BC26 和 BC27；
- 旧 VM 按现有规则拒绝 BC27；
- 编译器/AOT 默认继续输出 BC26；
- 只有实际发射 ext/direct 新 opcode 时才把 bundle 升为 BC27；
- cache key、host capability 和 worker identity 明确包含 bytecode format；
- 先滚动升级全部 reader/worker，再打开 BC27 emitter；
- 回滚时停止发射 BC27，已有 BC26 仍可执行。

这样可把 host/compiler/worker 同步 flag-day 改为可回滚的两阶段升级。若产品因
安全策略坚持单版本 reader，必须把停机窗口、缓存失效和回滚成本作为生产 keep
门，而不是仅在决策记录中描述。

### 8.3 Opcode 预算

`OP_ext=252`、253/254 两个 direct slot、255 永久非法的布局可以保留。escape
prefix 已解决容量问题；真正稀缺的是“一次 dispatch 的廉价 opcode”预算。

- property/call 等昂贵 handler 优先留在 ext 空间；
- tag-test-only 的廉价 numeric opcode 必须证明 ext 不回退；
- 253/254 只分给 direct-vs-ext 实测过门的候选；
- 若 fusion 比单 tag-elision 节省更多 dispatch，应允许 fusion 赢得 direct slot；
- 255 保留非法有利于 fail-closed，不需要为了占满预算而使用。

## 9. Profiling 与性能证据补充

### 9.1 profile build 与 production build 分离

handler 级计数、每 1024 次时钟采样、site 统计都会改变 code layout、分支预测
和 cache 行为。profile 只用于机会排序，最终收益必须用无 profiling 字段和分支
的 production build 测量。

采样时钟的固定开销需要通过空 handler/control opcode 校准；排名同时报告：

- dynamic executions；
- slow-path executions；
- sampled ticks share；
- specializable hit rate及其置信下界；
- transition/fusion 覆盖；
- profile build 自身相对 patchless baseline 的扰动。

### 9.2 训练与验证分离

用于选 site 的 profile 流量和用于 keep 的 workload 不能完全相同。至少采用：

- staging/train profile 决定发射；
- 固定 revision 的 quickjs microbench/web-tooling 和独立 Hono request mix 做
  validation；
- profile-stale、profile-missing、emitted-cold 单独进入矩阵。

恶意或损坏 profile 只能影响“选择哪个语义等价的 guarded opcode”，不能参与
Lane 1 静态证明。解析必须限制文件大小、site 数、计数溢出和重复 key，防止
不可信 profile 造成内存/编译时 DoS。

### 9.3 A/B 矩阵

使用同一会话、固定 CPU/governor、预热后至少 7 个成对样本，采用平衡的
ABBA/BAAB 顺序，报告 paired ratio 的几何均值、同号数和置信区间。至少比较：

```text
patchless baseline binary
feature code present but OFF
Lane 1
Lane 2A
Lane 2B（条件）
Lane 1 + Lane 2
ext vs direct
emitted-cold
```

`patchless` vs `OFF` 用来量出新增 VM handler 对代码布局/I-cache 的静态税；
`OFF` vs `ON` 才是候选的运行收益。确认的单 workload 回退超过 2% 必须修复或
trim，不能被几何均值掩盖。

## 10. 修订后的收益预期

基于当前 quickjs-ng handler 已有 int/float fast path，应把首轮收益先验下调，
避免用 sablejs 的 native/JIT 收益做量化外推：

| 机制 | 广谱先验 | 隔离热点先验 | 备注 |
| --- | ---: | ---: | --- |
| Lane 1 static tag-elision | 0%~3% | 3%~10% | ext 形态可能无收益，direct 才有机会 |
| Lane 2A stateless/fusion | 1%~5% | 5%~15% | fusion 通常比重复 generic tag guard 更可信 |
| Lane 2B sparse shape/call cache | 3%~10% | 10%~25% | 取决于热点覆盖、RSS 和回退分布 |
| 首轮组合 | **3%~8%** | — | 当前最可信的 broad keep 目标 |

`8%~15%` 可以保留为 stretch goal，但必须有 property/call/fusion 至少一类通过
动态成本、语义和 RSS 门。`15%~25%` 只能作为多轮理论上限，不应写成首轮承诺。

无论先验如何，正式 go/no-go 仍以 A2/A3/A4 的实际数据为准：若 generic handler
已经覆盖相同 fast path、ext dispatch 抵消收益、或静态证明覆盖不足，直接归档
no-go，不因已经建设 profiling 基础设施而升级 BC_VERSION。

## 11. 给原方案的最小修订清单

方案作者若不想重写全文，至少完成以下修改后再交给执行 AI：

1. 删除“call 族携带 function atom”和“JIT 看不到全程序”的表述；
2. 把过程间分析移到条件阶段，首轮 call result 为 unknown；
3. 把 P9 描述改成“已删除的设计参考”，明确 P17 要重建 stack-type dataflow；
4. 修正 `push_i16/push_const` 等事实源；
5. 将 Lane 2 拆为无状态 2A 和稀疏状态 2B，删除 exact shape 零状态承诺；
6. profile site key 改为 bundle digest + function cpool path + original pc/opcode；
7. 在 A2 与 B1 之间新增 candidate-specific A3 analyze-only/A4 裁决；
8. 增加 optimized-test262 adapter 和完整 quickjs-ng 原生测试命令；
9. P17/P18 首版移到现有 generic fixed point 之后；
10. 将生产升级改为 BC26/BC27 双读、条件写 BC27，或明确记录拒绝双读的成本；
11. 将首轮 broad 预期修正为 3%~8%，8%~15% 标为 stretch；
12. 只有 A4 至少一个候选过门才允许提交 BC27/OP_ext 基础设施。

完成这些修订后，两份姊妹方案的合理编排是：共享 A0-A2；A3/A4 先裁静态与
无状态可行性；再落 BC27 和 AOT Lane；最后仅对仍有足够 residual ticks 的热点
评估 sparse cache/runtime quickening。这一顺序保留了原方案最有价值的
validation-first 思路，同时避免在错误的调用模型或不可实现的零状态 shape
guard 上投入格式级成本。
