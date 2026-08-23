# quickjs-ng Opcode Profiling 与 AOT 静态证明融合方案(tier-3:CFG+SSI 优化器扩展)

> rev 2:整合技术评审(`quickjs-ng-opcode-profiling-aot-pgo-review.md`)的
> 全部修订 —— 六个实现前阻塞项的修正、Lane 2A/2B 拆分、PGO site
> identity 契约、A3 analyze-only 阶段、optimized-test262 硬门、
> BC26/BC27 灰度部署、测试矩阵与收益重估。评审文档与本文件配套归档,
> 其结论保留为决策记录。
> 本文件与 `quickjs-ng-deep-opcode-specialization.md`(VM 侧自适应专门化)
> 互为姊妹方案:共享 profiling 基础设施、BC27/OP_ext 格式决策与 G1-G5
> 门禁框架,但走不同的执行路径 —— 本方案优先 **AOT 静态证明 + 离线
> PGO**,运行时 quickening 只作为最后手段。执行前先确认基线至少包含
> P16 keep 裁决(当前文档基线:`d32378a`,P16:`40c3d8a`),并通读
> `docs/bytecode-aot-optimizer.md` §3、§5、§11。

## 0. 结论与预期

quickjs-ng 的算术/比较/属性访问在 interpreter 里是 dispatch + 逐操作数
tag 检查 + 溢出分支。与 sablejs 的关键差异是**定量而非定性的**:sablejs
把算术降为原生 JS 运算符后,V8 JIT 接着做寄存器分配、类型反馈与机器码
生成;quickjs-ng 新增专门化 opcode 后仍执行 boxed `JSValue`、interpreter
dispatch 与引用计数。因此 sablejs 的 +45% local promotion 等收益只支持
本方案的方法论,**不能作为 quickjs-ng 的收益先验**。

本方案三道防线,按风险递增排列:

| 防线 | 机制 | 运行时成本 | 失败模式面 |
| --- | --- | --- | --- |
| Lane 1:P17/P18 静态类型证明 | CFG 类型格 + 候选专属最小证明域,证明即发射,**无 guard** | 零(编译期) | 证明链本身的正确性 |
| Lane 2A:无状态静态 guard/fusion | tag/class/argc/可序列化 cpool identity/融合序列 | guard 一次检查,无任何状态 | 与 generic handler 同构的语义面 |
| Lane 2B:PGO 筛选的稀疏 cache | exact shape/offset/callee identity,仅 profile 热 site | 热 site 状态分配 | 上游 IC 的 GC/失效/megamorphic 面,但只覆盖热 site |
| Lane 3:运行时 quickening | 姊妹方案 §5(side table + score + ext_id 改写) | 热函数状态分配 | 上游 IC 全部失败模式(PR #884 删除原因) |

**收益先验(修订,已按评审下调)**:

| 机制 | 广谱先验 | 隔离热点先验 | 备注 |
| --- | ---: | ---: | --- |
| Lane 1 静态 tag-elision | 0%~3% | 3%~10% | ext 形态可能无收益,direct 才有机会 |
| Lane 2A 无状态/fusion | 1%~5% | 5%~15% | fusion 通常比重复 generic tag guard 更可信 |
| Lane 2B 稀疏 shape/call cache | 3%~10% | 10%~25% | 取决于热点覆盖、RSS 与回退分布 |
| 首轮组合 | **3%~8%** | — | 当前最可信的 broad keep 目标 |

`8%~15%` 是 stretch goal(需 property/call/fusion 至少一类过全部门);
`15%~25%` 只作多轮理论上限,不写成首轮承诺。正式 go/no-go 以 A2/A3/A4
实测为准:若 generic handler 已覆盖相同 fast path、ext dispatch 抵消收益、
或静态证明覆盖不足,直接归档 no-go,不因已建 profiling 基础设施而升级
BC_VERSION。

**不做**:JIT、baseline compiler、NaN-boxing 重构、16-bit decoded IR、
AST 级 HIR(维持"字节码即 HIR"的 v1 架构决策)、profile 驱动的指令重排
(破坏确定性)、v1 的调用内联。

## 1. 经验依据(本方案的纪律来源)

### 1.1 sablejs docs 的 AOT 经验(逐条采纳)

`sablejs/docs/optimization-plan.md` + `performance.md` 是同一方法论在
相邻技术栈上的完整执行记录,以下条目直接进入本方案门禁:

- **validation-first(Item 5 先例)**:Intrinsic-read LICM 在建 pass 前先
  跑 `--profile-boundary`,发现目标计数器会降 ~0,pass 判 no-go 未建。
  本方案 A2/A3 就是同类动作:先量再建,候选由数据选而不是先验指定。
- **try-measure-reject(Item 2 先例)**:静态身份 guard 实测覆盖 ≈0、
  per-site 成员 memo 实测 **-20%**,两个方案都被数据否决后才收敛。
  负结果照实归档,不因 sunk cost 保留。
- **配对交错 A/B 是唯一可信证据**:全文档所有结论都是 in-session
  interleaved A/B(median of 4-9);"absolute values drift ±20% between
  sessions, the in-session delta is the evidence"。首轮异常必须复测:
  DeltaBlue -5.5% 首测在六轮复检后证明是噪声(1,002 vs 1,008);-8.6%
  被证明是 800-iteration 窗口噪声(重测 +0.1~0.3%)。
- **每 pass 的 soundness 证明 + 具名回归 bug**:7a DSE 五个 soundness bug
  各自定位到机制缺陷并逐一回归钉死。本方案 P17 的每条传播/屏障规则
  按同一标准执行。
- **kill switch + stats 计数**:capsid 对应物是 PassFlags 位 + report
  逐 pass 行,直接沿用。
- **真实世界覆盖原则(Item 4)**:推荐部署路径决定了什么优化"每行代码
  收益最高"。capsid 对应物:26-bundle 语料 + Hono 请求 mix 是部署门
  输入(§10.4)。
- **上游 IC 删除教训(PR #884)**:quickjs-ng 删除了几何均值 +3.67% 的
  IC,因为 typescript/babel/babylon 回退、内存增长、维护负担。本方案
  的规避:2A 无状态(零内存、零 GC root、冷代码零税);2B 用 PGO 把
  上游全量 IC 改成**稀疏 IC**,并完整实现其 GC/失效/megamorphic 规则。

### 1.2 capsid 自身测量教训(2026-08-23 本会话)

sablejs v8-suite DSE A/B 复测:block 式 A/B 在负载漂移下造出 +52%/+27%
假阳性;顺序交替配对后收敛到 +2.6%(5 对中 4 对同号)。配对协议是本方案
默认测量协议(§10.3 固化为 ABBA/BAAB 平衡序)。

## 2. 现状锚点(2026-08-23,已对 vendor 源码核验)

### 2.1 capsid 优化器 as-built

- `kPassAll` = P2|P31|P11|P14|P16(0x1f);固定点 ≤16 轮,顺序
  P11/P14 → P16 → P2 → P3.1 → P6 re-shorten;P7 pc2line 重映射、
  P8 splice/checksum、验证器(`tools/bytecode_optimize.{h,cc}`)。
- 可复用资产:**P2 槽值格与屏障思想**、**P14 cpool tag/value 读取器**、
  **P16 的槽编号/读写分类/后向活性模型**、**verifier 的精确 stack
  height**、`FuncRecord.children` 的序列化 function cpool tree。
- **P9-P15' SSI 实现已由 commit `8078d04`(tier-2 G4 trim)删除**,源码中
  不存在可扩展的 φ/SSI 层;其文档仅作设计参考。现有 P2 只跨 BB 保存
  slot lattice,operand stack 类型在每个块内最多保存 top/prev ——
  **P17 必须重建完整的 per-BB 操作数栈类型向量与槽类型向量**,并与
  verifier 的 stack height 完全一致。

### 2.2 quickjs-ng 关键事实(评审阻塞项的核验结论)

- opcode 表(`quickjs-opcode.h` DEF 行):`push_i32`(5B)、`push_const`(5B,
  cpool tag 含 JS_FLOAT64/JS_STRING/JS_INT)、`push_atom_value`、
  `push_i8`/`push_i16`/`push_const8`、`push_0..7`/`push_minus1`、
  `push_bigint_i32`;**不存在 `push_16` 与 `push_f64`** —— F64 常量经
  `push_const` 的 cpool tag 识别。对象有 `OP_object`;数组字面量是构建
  序列 + `array_from`(3B,npop),**不存在单一 array producer opcode**。
- **`call/call_method/call_constructor/tail_call` 的 u16 操作数是
  `argc`,不携带 function atom、function slot 或 child index**;解释器
  从操作数栈取 callee(已核验 `quickjs.c` OP_call 与 OP_add handler)。
  只有 `fclosure/fclosure8` 自身携带 cpool index。
- generic 算术 handler 已有快路径(已核验 OP_add):`likely(JS_VALUE_IS_BOTH_INT)` 第一分支、float-float 第二、slow 走 `js_add_slow`
  —— **带同样 tag guard 的新 opcode 不消除 tag check,只改变分支布局**;
  Lane 1 静态证明的价值是**完全跳过** tag 检查,两者不可混为一谈。
- Capsid 集成只经 `patches/txiki/` overlay:36 个既有 patch 与
  `quickjs-opcode.h`/dispatch/reader 区域的冲突地图是 A0 交付物。

## 3. Lane 1:P17 类型证明 + P18 无 guard 静态发射

### 3.1 candidate-specific 最小证明域(修订)

不在候选确定前建设完整类型格。A2 选出候选后,A3 只建设支持候选的最小
证明域。例如候选是 `add_i32i32` 时:

```text
BOTTOM       不可达/尚无前驱
EXACT_INT    运行时必为 JS_TAG_INT
UNKNOWN      任意 JSValue
```

```text
join(BOTTOM, x)      = x
join(EXACT_INT, EXACT_INT) = EXACT_INT
join(any other pair) = UNKNOWN
```

实现必须维护每个 basic block 的完整 operand-stack 类型向量和 slot 类型
向量(与 verifier 的 stack height 一致)。每个 serialized opcode 都必须有
transfer 分类;**未分类 opcode 默认把受影响栈值与可见槽降为
UNKNOWN/barrier,而不是继续传播**。

### 3.2 事实源(修正后,全部编译期可证,规则逐条带证明义务)

| # | 事实源 | 导出类型 | 证明依据 |
| --- | --- | --- | --- |
| 1 | push_i32/push_i8/push_i16、push_0..7/push_minus1 | EXACT_INT | 生产 opcode 语义 |
| 2 | push_const | cpool atom 种类 | P14 读取器已验证快照只含数据属性;JS_TAG_INT→EXACT_INT,JS_TAG_FLOAT64/STRING→UNKNOWN(首轮最小域),未知种类→UNKNOWN |
| 3 | push_true/false/null/undef | UNKNOWN(首轮) | 最小域只关心 EXACT_INT |
| 4 | OP_object / array_from 构建序列 | UNKNOWN(首轮) | 形状不静态证 |
| 5 | P2 折叠常量 | 折叠即 push 该常量 | P2 的 K_* 自然升级为对应域值 |
| 6 | eq/lt/le/gt/ge | 结果 UNKNOWN(首轮) | 比较结果恒为 boolean,但首轮最小域无 T_BOOL;域扩充由 A3 按候选决定 |
| 7 | add/sub/mul/div/mod/inc/dec/add_loc/neg/plus | 结果 UNKNOWN | 溢出依赖值,静态不可判 —— **结果保守,输入前提照常可用** |
| 8 | put_loc/get_loc 槽传播 | 传递 | 槽即寄存器;P16 已证明槽编号模型正确 |
| 9 | **call 族调用结果** | **一律 UNKNOWN(首轮)** | call 操作数只有 argc,callee 来自栈;过程间传播移条件阶段(§3.4) |

### 3.3 屏障规则(每条配回归测试)

- captured slot(vardef 0x40)、eval/with/special-object → 全槽 UNKNOWN
  (P2/P16 同款屏障,复用其判定);
- put_loc_check/put_loc_check_init:定义后类型可传播(TDZ 是值语义,
  P16 liveness 独立保障);
- get_field(非 P14 折叠)、get_array_el、属性写后的对象 → UNKNOWN;
- **异常路径**:当前 CFG 没有 exception edge。首版含 `catch/gosub`、
  try/finally 或动态环境的 function **整函数不发射**(sablejs DSE 发现
  try/finally soundness bug 后整 scope 跳过的先例);有独立异常 CFG 后再
  评估"handler 入口强制 UNKNOWN"的替代方案;
- **未知输入 → 不发射**:site 处任何操作数非精确匹配 → 原 opcode 保持。
  每条规则配"渲染路径证明 + 回归测试"并进入 differential fuzz 语料。

### 3.4 过程间分析:移为条件阶段(修订)

首轮 P17 不做一般过程间返回类型传播,把调用结果一律设为 UNKNOWN,先验证
函数内静态数值证明是否有足够动态覆盖。只有 A2 显示 call/return 事实会
显著增加已选候选覆盖时,才增加独立的 closure identity lattice:

```text
F_EXACT(function_cpool_path)
F_UNKNOWN
```

进入 F_EXACT 至少要求:生产者是当前 bundle 中可定位的
`fclosure/fclosure8`;所有传播路径保持同一 function cpool path;不经过
属性/global/var_ref/未知调用返回;closure 未逃逸到 host、export、动态
属性或未知容器;callee 存在未知调用方时,参数类型与环境依赖事实全部
UNKNOWN;eval、with、动态 import、host callback 和无法证明的递归 SCC
直接降级。**"bundle 中能读所有 function bytecode"不等于 closed world**:
导出入口、host 回调、间接属性调用与动态代码仍能产生 bundle 外调用方。

### 3.5 P18 发射清单(候选先验序,A2 重排)

| 候选 | 前置条件 | 收益机制(相对已核验的 generic handler) |
| --- | --- | --- |
| add/sub/mul/div/mod_i32i32 | 栈顶双 EXACT_INT | **跳过** `JS_VALUE_IS_BOTH_INT` 的 tag 提取与分支(guard 形态做不到这点),直达 int64+溢出路径,复用同一 overflow/浮点慢路径 |
| inc/dec/add_loc_i32 | 槽 EXACT_INT | 同上,槽内 |
| neg/plus_i32 | 栈顶 EXACT_INT | 同上 |
| eq/lt/le/gt/ge_i32i32 | 栈顶双 EXACT_INT | 整数快速比较 |

get_field 不在清单(P14 已覆盖字面量对象,运行时形状留给 Lane 2B)。
call 不在清单(v1 不做内联;argc 证明的收益面由 A2 决定是否进 2A)。

## 4. Lane 2:PGO 定向发射(拆分为 2A/2B,修订)

### 4.1 Lane 2A:无状态静态 guard/fusion

允许的候选:tag、class、argc、**可序列化的 cpool identity**、融合序列。
guard 是字节码里的检查(miss → generic 慢路径),**没有任何运行时状态、
side table、GC root 或 score** —— 上游 IC 的内存与维护面不存在。

numeric 候选进入 2A 的附加条件(评审 §4.1):generic handler 已把 int-int
放在 likely 第一分支,带同样 tag guard 的新 opcode 不消除 tag check。
因此 numeric Lane 2 **不能因 `i32_i32 stable ≥90%` 自动进入原型**;必须先
证明它相对 generic handler 真正少执行了什么,并做 patchless/OFF/ON 三态
A/B。float-first 重排序、跨多条指令共享一次 guard 或 fusion 才是较可信的
无状态候选。

### 4.2 Lane 2B:PGO-seeded 稀疏 cache(条件)

exact shape、property offset、callee identity 的比较对象是运行时对象
(`JSShape*` 指针、closure identity),profile 中的地址不能跨进程写入
bundle;按 atom/property layout 重新结构化验证的成本接近通用查找,不能
等价替代 exact identity。**"exact shape guard 且零运行时状态"不成立**,
故此类候选进入 2B:

- 仅在 profile 热 site 发射;**冷 site 没有字段、root 或分配**;
- 热 site 的状态上限、GC mark/free、函数销毁移除、失效与 megamorphic
  规则必须完整实现和测试(上游 IC 的全部失败模式,不因稀疏而豁免);
- 2B 的价值不是"没有 IC",而是把上游全量 IC 改为 PGO 筛选后的稀疏 IC。

若产品红线仍是绝对零运行时状态,则 named-property/callee 候选从 Lane 2
删除,只保留 2A,并接受收益上限明显下降。

### 4.3 发射策略与 emitted-cold

发射决策 = Lane 1 证明(无条件)∪ 2A/2B 的 profile 热 + 命中率达标
(带 guard);其余 site 保持原 opcode —— 冷代码零税。"emitted-cold"
(已发射但 profile 证明从不命中的 site)作为独立状态进 §10.3 矩阵,
门 <1%。

## 5. PGO site identity 与产物契约(修订)

`function atom + pc` 不能唯一标识 site:匿名/同名函数会碰撞,atom 不是
function identity,运行内序号未必能跨 bundle 装载顺序重现。profile v1 的
site key 固定为:

```text
prePgoBundleSha256
moduleOrdinal
functionCpoolPath[]      // 从根 function 按 cpool 序列化顺序进入 child 的 index path
originalPc
originalOpcode
```

采集与消费必须使用同一个 canonical pre-PGO bundle:

```text
source
  -> 当前 kPassAll,输出 canonical BC26 bundle B
  -> profiling worker 执行 B,profile 绑定 sha256(B)

source + profile
  -> 重新生成 B
  -> bit-for-bit 校验 sha256(B)
  -> 以 functionCpoolPath + originalPc 匹配 site
  -> 才运行 P17/P18/PGO 发射
```

任何 digest、build identity、opcode 或 function path 不匹配都 fail closed
—— P11/P14/P16/P2 删除和改写指令后,不会把旧 profile 错配到新 PC。
`stable ≥90%` 必须绑定最小 hit 数与置信度:建议 Wilson lower bound
≥0.90 并预登记最小 hit 数,不能让 `9/10` 与 `900000/1000000` 等价。

## 6. Lane 3:运行时 quickening(最后手段)

仅当 Lane 1+2 全部落地、重采样后仍有 ≥2% tick 覆盖且命中率 ≥90% 的残余
热点,才启用姊妹方案 §5 的 quickening/side table/score 设计,并追加两条:

1. `quicken_score` 属于序列化 struct 的 bitfield —— writer 强制清零、
   reader 忽略,并加"score 非零的运行后重序列化 == 未执行字节"的定向
   测试;
2. OFF/ADAPTIVE 在 test262-fast 上的可观测等价显式入门禁。

## 7. 格式、opcode 空间与灰度部署(修订)

### 7.1 格式决策(与姊妹方案共享)

BC27 + OP_ext(=252,prefix+ext_id+payload)+ 253/254 direct slot + 255
永久非法的混合布局、ExtOpInfo 表、secondary dispatch、reader
canonical-only、writer canonicalize、静态断言组 —— 采用姊妹方案 §4
设计,不重复叙述。两处修正:

1. **direct slot 晋升评估时点**:在 Lane 1 静态发射落地后重新计算剩余
   份额;Lane 1 无 guard 形态 handler 最短、二级 dispatch 相对占比最大,
   是 253/254 第一候选;
2. **opcode 预算纪律**:property/call 等昂贵 handler 优先留在 ext 空间;
   tag-test-only 的廉价 numeric opcode 必须证明 ext 不回退才发射;若
   fusion 比单 tag-elision 节省更多 dispatch,允许 fusion 赢得 direct
   slot;255 保持非法,不为了占满预算分配。

### 7.2 BC26/BC27 灰度,而不是 flag-day(修订)

- 新 VM **同时接受 BC26 和 BC27**;旧 VM 按现有规则拒绝 BC27;
- 编译器/AOT 默认继续输出 BC26;**只有实际发射 ext/direct 新 opcode 时
  才把 bundle 升为 BC27**;
- cache key、host capability 和 worker identity 明确包含 bytecode format;
- 先滚动升级全部 reader/worker,再打开 BC27 emitter;回滚 = 停止发射
  BC27,已有 BC26 仍可执行;
- 上游版本冲突:quickjs-ng 上游未来自升 BC_VERSION 可能与 27 冲突 ——
  升级流程必须 re-baseline + 静态断言兜底;capsid 兼容性 ID 本就隔离
  跨 vendor 字节码,不依赖裸版本号。

若产品因安全策略坚持单版本 reader,必须把停机窗口、缓存失效和回滚成本
作为**生产 keep 门**,而不是仅在决策记录中描述。

## 8. 管线集成(修订)

```text
现有 generic fixpoint(P11/P14/P16/P2/P3.1/P6,≤16 轮)
  -> P17 candidate-specific type proof(单次运行)
  -> P18 specialization emission(单次运行)
  -> ext-aware verifier
  -> P7 pc2line
  -> P8 splice/checksum
```

**不要在固定点早期把 generic 指令替换成 ext opcode**:P2/P3.1 可能不再
识别可折叠指令。只有实验证明 specialized opcode 能继续喂给现有 pass、
且所有 pass 都有 ext transfer/classification 后,才考虑并入固定点。
其余不变量:P17 格有限只升不降 → 单调;P18 只做替换(指令数不增);
未分类 ext → barrier;验证器扩展 ext 解码、跳转目标不落入 payload、
栈高不变量;报告仅 stderr、fail-closed、确定性红线不变。

## 9. 正确性门禁(修订:optimized-test262 是独立硬门)

### 9.1 quickjs-ng 原生主门禁

每次改动:`make jscheck`、`make ctest`、`make cxxtest`、`make`、
`make test`、`./build/api-test`、`./build/lre-test`、
`./build/qjs tests/test_bjson.js`、`make test262-fast`。候选 keep、
格式变更或 reader/writer/dispatch 变更前:`make test262` +
`make test262-check`。不得修改 `test262_errors.txt` 掩盖新增失败。
覆盖 Debug/Release、GCC/Clang、`JS_NAN_BOXING=0/1`、
computed-goto/switch、standalone/parserless、Linux ASan+UBSan
(可用环境继续 TSan/Valgrind 与多平台 CI)。

### 9.2 optimized-test262 adapter(修订)

普通 `run-test262` 从源码直接进入 quickjs 编译器与 VM,**不会自动调用
Capsid 的 serialized-bytecode optimizer**。必须增加 test262 AOT adapter:

```text
runtime-positive test262 case
  -> quickjs 编译并序列化
  -> Capsid kPassAll + P17/P18/PGO
  -> BC27 reader
  -> 执行并交回 run-test262 adjudication
```

执行模式矩阵:

| 模式 | 目的 |
| --- | --- |
| source baseline | 未优化 quickjs 语义 + parse-negative 用例 |
| BC26 serialize/deserialize | 旧格式与 round-trip 基线 |
| BC27 generic/no ext | 双版本 reader 与空变换 |
| Lane 1 proof emission | 真实 P17/P18 发射路径 |
| Lane 2 synthetic-hot | 合成 profile 强制 guard opcode 发射 |
| guard-hit/guard-miss | 两条路径都必须可达且与 generic 等价 |
| OP_ext/direct | 同一 handler 的两种 dispatch 形态 |
| Lane 3 OFF/ADAPTIVE | 若启用,quickening 前后可观测等价 |

parse-negative 仍由 source baseline 裁决;可序列化的 runtime/module/
async/异常语义用例进入 optimized 路径。test262-fast 用于每次迭代,
所有最终保留候选至少在 release 配置上跑一次 full optimized-test262。

### 9.3 新格式定向测试

BC26/BC27 合法读取与旧 VM 拒绝 BC27;ext_id 0、未定义 id、截断 payload、
错误 size/format、target 落入 payload;atom/slot/label payload 的 endian
rewrite;runtime-only ext state 禁止从 bundle 读取;执行后重新序列化
canonicalize 回稳定 bundle;direct/ext、switch/computed-goto 行为一致;
overflow、`-0`、NaN、BigInt、Symbol、对象 coercion、getter/Proxy、异常与
pc2line;Lane 2B/Lane 3 若启用时的 GC mark/free、函数销毁、megamorphic
与 OOM。Capsid RED/round-trip/differential/fuzz 40k 是第二层集成门,
不能替代以上原生门禁与 optimized-test262。

## 10. 性能与资源裁决(修订)

### 10.1 profile 与 production 分离 + 训练验证分离

handler 级计数、采样与 site 统计会改变 code layout、分支预测与 cache
行为。profile 只用于机会排序,**最终收益必须用无 profiling 字段和分支的
production build 测量**。采样时钟的固定开销用空 handler/control opcode
校准;排名同时报告动态次数、slow-path 次数、sampled ticks share、
specializable hit rate 及其置信下界、transition/fusion 覆盖、profile
build 自身相对 patchless baseline 的扰动。

用于选 site 的 profile 流量与用于 keep 的 workload 不能完全相同:
staging/train profile 决定发射;固定 revision 的 quickjs microbench/
web-tooling 与独立 Hono request mix 做 validation;profile-stale、
profile-missing、emitted-cold 单独进矩阵。**恶意或损坏 profile 只能影响
"选择哪个语义等价的 guarded opcode",不能参与 Lane 1 静态证明**;解析
必须限制文件大小、site 数、计数溢出与重复 key,防不可信 profile 造成
编译时 DoS。

### 10.2 A3 analyze-only 与 A4 裁决(修订)

原顺序要求 A2 报告"Lane 1 静态可证明份额",但 P17 到 C1 才实现。修订为
候选选出后先做 **A3 analyze-only**:对候选建立最小证明域,只统计可证明
动态份额,**不发射、不改字节码**。A4 结合动态成本、A3 覆盖与 handler
理论差分(相对已核验的 generic fast path 真正少执行了什么)裁决是否值得
新格式。**A4 至少一个候选过门才允许提交 BC27/OP_ext 基础设施。**

### 10.3 A/B 矩阵与协议(修订)

同会话、固定 CPU/governor、预热后至少 7 个成对样本,**ABBA/BAAB 平衡
顺序**,报告 paired ratio 的几何均值、同号数与置信区间。至少比较:

```text
patchless baseline binary      // 新增 VM handler 的代码布局/I-cache 静态税
feature code present but OFF   // OFF vs patchless = 静态税;OFF vs ON = 运行收益
Lane 1 / Lane 2A / Lane 2B(条件)/ Lane 1 + Lane 2
ext vs direct
emitted-cold
```

确认的单 workload 回退 >2% 必须修复或 trim,不能被几何均值掩盖。

### 10.4 门禁汇总 + 部署门

- G1:§9 全部(quickjs 原生 + optimized-test262 硬门 + capsid 集成);
- G2:PGO-off 与今日 kPassAll 输出逐字节一致(构造性 + 8 个 no-trigger
  fixture sha256 门);emitted-cold <1%;
- G3:全性能语料几何均值 ≥2%(§10.3 协议),至少两个 lane ≥5%,任何
  lane 回退 >2% 交错复测确认后 trim;
- G4:逐候选归因(P17 单独、P18 按 opcode 族、2A/2B、direct slot),
  <1% → trim;
- G5:ophist 前后、dispatch 地板、profile 覆盖率 vs 收益相关图;
- **部署门**:引擎达标 ≠ capsid 升级。26-bundle 语料与 Hono 请求 mix
  逐项无 >2% 回退(对应上游 typescript/babel 教训),灰度方案(§7.2)
  的滚动升级与回滚演练通过,才允许生产 emitter 打开 BC27。

## 11. 实施顺序与提交边界(修订)

1. **A0 基线**:固定 vendor/build/CPU、原生测试、test262、性能和
   **36 个 overlay patch × opcode 表/dispatch/reader 区域的冲突地图**;
2. **A1 profiling**:只加 profile build,不改生产 VM/BC_VERSION;采集
   (quickjs microbench + web-tooling 固定 revision + capsid 26-bundle
   语料在 worker 变体上);
3. **A2 dynamic ranking**:sampled cost、slow-path share、动态次数、
   稳定率(含置信下界)选出最多两个候选;
4. **A3 analyze-only proof**:对候选建立最小类型/身份域,只统计可证明
   动态份额,不发射、不改字节码;
5. **A4 go/no-go**:动态成本 + A3 覆盖 + handler 理论差分 → 是否值得
   新格式;
6. **B1 BC27/OP_ext**:仅 A4 至少一个候选通过才开始;
7. **C1 Lane 1**、**C2 Lane 2A**、**C3 条件 Lane 2B**(各带完整
   §9 门禁);
8. **D1 条件 Lane 3**:仅剩余热点仍达标时开始 runtime quickening;
9. **D2 收尾**:证据归档(含负结果)、docs/architecture/performance
   同步、opcode/direct slot 占用记录、灰度演练记录。

每阶段单独 commit,禁止跨阶段混交。A1 虽修改 profiling build 的 VM 源码,
但不改变生产 binary、生产 bundle 或字节码 identity —— "不动 VM"的准确
表述是"**不动生产 VM/格式**"。

## 12. 风险与强制停止条件(修订)

| 风险 | 缓解 |
| --- | --- |
| 静态证明链某条规则错 → 无 guard 错发射 | 保守屏障 + 每规则回归测试 + optimized-test262 硬门;UNKNOWN 一律不发射 |
| 异常路径不可见(无 exception edge) | 首版含 catch/gosub/try-finally 的 function 整函数不发射 |
| numeric guard 形态无收益(generic 已有 likely int-int) | 2A 加"证明真正少执行什么"前置 + patchless/OFF/ON 三态 A/B |
| exact-shape 零状态承诺不成立 | 已拆 2B(稀疏状态);产品红线若禁止状态则删 2B |
| profile 引入非确定性 / site 错配 | §5 产物契约:canonical B + sha256 校验 + cpool path site key,fail-closed |
| 恶意 profile 编译时 DoS | 解析限额(文件大小/site 数/计数溢出/重复 key);profile 不进静态证明 |
| ext 二级 dispatch 吃掉收益 | direct 晋升按 Lane 1 落地后剩余份额评估;廉价 opcode 证明 ext 不回退 |
| 新 opcode 破坏既有 pass | 固定点外单次运行 + 全量分类 + barrier 兜底 + golden 字节测试 |
| 上游 BC_VERSION 冲突 | 升级 re-baseline + 静态断言;兼容性 ID 隔离 |
| 维护成本(上游 IC 第三删除理由) | keep 报告含代码增量账与每 site 状态账;2A 天然零状态,2B 稀疏 |
| 所有 lane 都不达标 | A4 no-go:归档 profiling 与 no-go 证据,不升 BC_VERSION |

## 13. 与姊妹方案的执行编排(修订)

共享 A0-A2;A3/A4 先裁静态与无状态可行性;A4 过门后才落 BC27 与 AOT
Lane;最后仅对仍有足够 residual ticks 的热点评估 2B/Lane 3 的稀疏
cache 与 runtime quickening。这一顺序保留了 validation-first 思路,
同时避免在错误的调用模型(call 族无 function atom)或不可实现的零状态
shape guard 上投入格式级成本。
