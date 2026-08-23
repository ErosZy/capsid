# quickjs-ng Opcode Profiling 与 AOT 静态证明融合方案(tier-3:CFG+SSI 优化器扩展)

> 本文件与 `quickjs-ng-deep-opcode-specialization.md`(VM 侧自适应专门化)
> 互为姊妹方案:共享 profiling 基础设施、BC27/OP_ext 格式决策与 G1-G5
> 门禁框架,但走不同的执行路径 —— 本方案优先 **AOT 静态证明 + 离线 PGO**,
> 运行时 quickening 只作为最后手段。执行前先确认基线至少包含 P16 keep
> 裁决(当前文档基线:`d32378a`,P16:`40c3d8a`),并通读
> `docs/bytecode-aot-optimizer.md` §3、§5、§11。

## 0. 结论与预期

quickjs-ng 的算术/比较/属性访问在 interpreter 里是 dispatch + 逐操作数
tag 检查 + 溢出分支;这与 sablejs 的基线事实正好相反 —— sablejs 的算术
在 codegen 层已是原生 JS 运算符(V8 代做 tag 检查),所以它的剩余杠杆是
frame promotion(+45% NavierStokes),而不是 opcode 专门化。**quickjs-ng
没有这层"原生化",opcode 专门化就是 sablejs 早已做完的那一步** ——
这是本方案与姊妹方案的共同目标面,差别在实现路径。

本方案三道防线,按风险递增排列:

| 防线 | 机制 | 运行时成本 | 失败模式面 |
| --- | --- | --- | --- |
| Lane 1:P17/P18 静态类型证明 | CFG+SSI 类型格 + 过程间传播,证明即发射,**无 guard** | 零(编译期) | 证明链本身的正确性 |
| Lane 2:离线 PGO 定向发射 | profile 热 site 发射**带 guard 的静态 adaptive 形态**(guard 在字节码里,无 side table/GC/score) | guard 一次 tag 检查 | 与上游 IC 同构的 guard 语义,但无内存/维护面 |
| Lane 3:运行时 quickening | 姊妹方案 §5(side table + score + ext_id 改写) | 热函数状态分配 | 上游 IC 全部失败模式(PR #884 删除原因) |

**诚实预期(先验,由 A2 实测裁决)**:Lane 1 的可证明份额可能只覆盖热点
20-40%(double 路径、对象形状、跨函数返回值都难以静态证);Lane 2 才是
广谱收益主力。若 A2 显示 Lane 1 覆盖 < 热点 15%,Lane 1 降级为
"P2 格的免费副产品"(P17 只做传播、不发射),直接推进 Lane 2。首轮 keep
目标与姊妹方案一致:全性能语料几何均值 ≥2%,至少两个 lane ≥5%,确认后
任何 lane 不得回退 >2%,峰值 RSS 增量 ≤1%。

**不做**:JIT、baseline compiler、NaN-boxing 重构、16-bit decoded IR、
AST 级 HIR(维持"字节码即 HIR"的 v1 架构决策)、profile 驱动的指令重排
(破坏确定性)、v1 的调用内联。

## 1. 经验依据(本方案的纪律来源)

### 1.1 sablejs docs 的 AOT 经验(逐条采纳)

`sablejs/docs/optimization-plan.md` + `performance.md` 是同一方法论在
相邻技术栈上的完整执行记录,以下条目直接进入本方案门禁:

- **validation-first(Item 5 先例)**:Intrinsic-read LICM 在写 pass 之前
  先跑 `--profile-boundary`,发现目标计数器会降 ~0,pass 判 no-go 未建。
  本方案 A2 就是同类动作:先量再建,候选由数据选而不是先验指定。
- **try-measure-reject(Item 2 先例)**:静态身份 guard 实测覆盖 ≈0、
  per-site 成员 memo 实测 **-20%**,两个方案都被数据否决后才收敛到
  arity 特化 dispatch。负结果照实归档,不因 sunk cost 保留。
- **配对交错 A/B 是唯一可信证据**:全文档所有结论都是 in-session
  interleaved A/B(median of 4-9);"absolute values drift ±20% between
  sessions, the in-session delta is the evidence"。首轮异常必须复测:
  DeltaBlue -5.5% 首测在六轮复检后证明是噪声(1,002 vs 1,008);-8.6%
  被证明是 800-iteration 窗口噪声(重测 +0.1~0.3%)。本方案 G3 门把此
  纪律写成协议(§8.2)。
- **每 pass 的 soundness 证明 + 具名回归 bug**:7a DSE 五个 soundness bug
  各自定位到机制缺陷(optimized-read kill 语义、DELLOCAL no-op、
  reuse-dangling-source、try/finally 异常路径等)并逐一回归钉死。
  本方案 P17 的每条传播/屏障规则按同一标准执行(§3.3)。
- **kill switch + stats 计数**:每个 shipped pass 都有 `--no-*` 开关与
  计数器。capsid 对应物是 PassFlags 位 + report 逐 pass 行,直接沿用。
- **真实世界覆盖原则(Item 4)**:推荐部署路径(Babel/SWC → ES5.1)决定了
  什么优化"每行代码收益最高"。capsid 对应物:26-bundle 语料 + Hono 请求
  mix 是部署门输入(§8.4)。
- **上游 IC 删除教训(姊妹方案 §1 引用)**:quickjs-ng PR #884 删除了
  几何均值 +3.67% 的 IC,因为 typescript/babel/babylon 回退、内存增长、
  维护负担。本方案 Lane 1/2 与 IC 的差异正是这三个失败项的规避:
  无 per-site 状态(零内存增量)、无 GC root、无冷函数税(§4.3)、
  静态 guard 的维护面远小于运行时 cache。

### 1.2 capsid 自身测量教训(2026-08-23 本会话)

sablejs v8-suite DSE A/B 复测:block 式 A/B(先跑完 3 轮 baseline 再跑
3 轮 DSE)在负载漂移下造出 +52%/+27% 假阳性;改为顺序交替配对后收敛到
+2.6% 几何均值(5 对中 4 对同号,落后对 -2.0% 在噪声带)。**配对协议不是
可选项,是本方案的默认测量协议**,与 §1.1 的 sablejs 纪律互相印证。

## 2. 现状锚点(2026-08-23)

### 2.1 capsid 优化器 as-built

- `kPassAll` = P2|P31|P11|P14|P16(0x1f);固定点 ≤16 轮,顺序
  P11/P14 → P16 → P2 → P3.1 → P6 re-shorten;P7 pc2line 重映射、
  P8 splice/checksum、验证器(`tools/bytecode_optimize.{h,cc}`)。
- 存量资产与本方案直接相关:
  - **P2 跨 BB 常量格**:已有 per-slot K_INT/K_ATOM 等值事实与屏障规则
    (with/eval/写变量收窄) —— P17 类型格的自然底座;
  - **P14 cpool 值读取器**:已证明序列化快照的自有数据属性可安全读
    (tag 精确 extent、fail-closed) —— P17 的 T_* 事实源之一;
  - **P16 后向槽活性**:read+write 指令 gen-win-over-kill 的精确分类
    模型 —— P17 复用同一 CFG 与槽编号;
  - **P9 SSI 构建/降回设计(tier-2 已建已裁)**:join 点栈驻留值命名 +
    φ 一致性验证器 —— P17 跨 BB 类型 join 的现成模板;
  - PassFlags/opt_mask 开关矩阵、40k fuzz 驱动、exec-throughput 配对
    测量脚本 —— G1/G4 基础设施全部现成。

### 2.2 quickjs-ng 代码锚点(以姊妹方案 §2 为准,补充本方案要用到的)

- opcode 表 `quickjs-opcode.h` DEF 行:算术/比较是单字节无操作数栈操作
  (`neg/plus/dec/inc/mul/div/mod/add/sub/lt/gt/eq/...` 均为 `DEF(x,1,*,*,none)`);
  `add_loc` 2 字节 loc8;`get_field` 5 字节 atom;`call` 族携带 function
  atom 或 slot(过程间分析的事实通道);
- direct-threaded dispatch(`dispatch_table[256]`,computed goto)与
  switch 构建并存,两种形态都必须过门禁;
- `JSFunctionBytecode` bitfield 尚有 5 bit(Lane 3 若启用才用);
- Capsid 集成仍只经 `patches/txiki/` overlay:36 个既有 patch 与
  `quickjs-opcode.h`/dispatch/reader 区域的冲突地图是 A0 交付物。

## 3. Lane 1:P17 类型格 + P18 无 guard 静态发射

### 3.1 类型格

```text
T_INT, T_F64, T_STR, T_BOOL, T_UNDEF, T_NULL, T_OBJ, T_ARR, T_UNKNOWN
join: 相同类型 → 自身;不同 → T_UNKNOWN
```

刻意不设 T_NUM 超类型:专门化的前置条件只需要**精确**类型,join 变粗
只意味着少发射,不意味着错发射 —— 格的设计目标是最小化"可能错"的
概率,而不是最大化覆盖。

### 3.2 事实源(全部编译期可证,规则逐条带证明义务)

| # | 事实源 | 导出类型 | 证明依据 |
| --- | --- | --- | --- |
| 1 | push_i32/push_i8/push_16、push_0..7 | T_INT | 生产 opcode 语义 |
| 2 | push_f64 | T_F64 | 同上 |
| 3 | push_const | cpool atom 种类 | P14 读取器已验证快照只含数据属性,atom tag 可精确读;未知种类 → T_UNKNOWN |
| 4 | push_true/false/null/undef | T_BOOL/T_UNDEF/T_NULL | 生产 opcode 语义 |
| 5 | object/array 字面量 | T_OBJ/T_ARR | 同上 |
| 6 | P2 折叠常量 | 对应类型 | 折叠即 push 该常量,P2 的 K_* 自然升级 |
| 7 | P14 折叠的 get_field | cpool 属性值类型 | P14 的折叠前提(自有数据属性、无中间写入)同时证明值类型 |
| 8 | eq/lt/le/gt/ge | 结果 T_BOOL | 比较结果恒为 boolean(与输入类型无关) |
| 9 | add/sub/mul/div/mod/inc/dec/add_loc/neg/plus | 结果 T_UNKNOWN | 溢出依赖值,静态不可判 —— **结果保守,输入前提照常可用** |
| 10 | put_loc/get_loc 槽传播 | 传递 | 槽即寄存器;P16 已证明槽编号模型正确 |
| 11 | call 族过程间 | callee return join | call 携带 function atom,bundle 内 callee 字节码全可见(§3.4) |

### 3.3 屏障规则(P17 的保守边界,每条配回归测试)

- captured slot(vardef 0x40)、eval/with/special-object → 全槽 T_UNKNOWN
  (P2/P16 同款屏障,直接复用其判定);
- put_loc_check/put_loc_check_init:定义后类型可传播(TDZ 是值语义,
  P16 的 liveness 已独立保障;类型传播不穿过未定义读);
- get_field(非 P14 折叠)、get_array_el、属性写后的对象形状变化
  → T_UNKNOWN(形状不可静态证,留给 Lane 2 的 guard 形态);
- 跨函数:SCC/递归 → 不动点 + 深度上限 1,超限 T_UNKNOWN;无法定位
  callee 原子 → T_UNKNOWN;
- **未知输入 → 不发射**:P18 只在 site 处栈顶两操作数类型精确匹配时
  发射,任何 T_UNKNOWN 参与 → 原 opcode 保持。静态证明链的每一条
  规则按 sablejs 7a 纪律配"渲染路径证明 + 回归测试",并进入
  differential fuzz 语料。

### 3.4 过程间分析(bundle 全可见是 AOT 的结构性优势)

JIT 见不到全程序,Lane 1 能:call/call_method 族携带 function atom,
bundle 内 callee 的返回类型 join 到调用点;递归/互递归 SCC 用不动点,
深度上限 1(超出即 T_UNKNOWN)。先验上这是 Lane 1 对 Hono 类 bundle
(工厂函数返回配置对象 → T_OBJ;validator 返回 bool → T_BOOL)的最大
单一覆盖来源。风险是分析面膨胀,缓解是深度上限 + SCC 保守 + 只对
profile 显示热的调用边展开(冷边默认 T_UNKNOWN)。

### 3.5 P18 发射清单(候选先验序,A2 重排)

| 候选 | 前置条件 | 收益机制 |
| --- | --- | --- |
| add/sub/mul/div/mod_i32i32 | 栈顶双 T_INT | 跳过逐操作数 tag 检查与 string/double 分支,直达 int64+溢出路径(与 generic handler 的 int-int 分支逐语义一致,复用同一 overflow helper) |
| inc/dec/add_loc_i32 | 槽 T_INT | 同上,槽内 |
| neg/plus_i32 | 栈顶 T_INT | 同上 |
| eq/lt/le/gt/ge_i32i32 | 栈顶双 T_INT | 整数快速比较(比较结果 T_BOOL,可继续传播) |

get_field 不在清单:字面量对象由 P14 已覆盖,运行时形状留给 Lane 2。
call 不在清单:v1 不做内联,argc 证明的收益面由 A2 决定是否进 Lane 2。

## 4. Lane 2:Opcode Profiling 基础设施与离线 PGO

### 4.1 profiling 补丁(与姊妹方案共享一份)

`CONFIG_OPCODE_PROFILE` 编译门,只存在于 profiling overlay build;
未定义时零字段/零分支/零符号(release 零开销用生成代码与 sizeof 对照
证明)。计数内容与 JSON schema 采用姊妹方案 §3 的
`quickjs-ng-opcode-profile-v1`(单 schema 双消费者):opcode 动态次数 +
类分布 + slow-path 次数 + 采样 tick + transition top-256 + site 级
(function atom + pc offset,命中数、类稳定率)。不写地址/路径/atom 文本;
function 用运行内稳定序号。写文件失败必须非零退出(fail-closed)。

### 4.2 Capsid 侧消费(本方案独有)

- worker profiling 变体从 staging 流量收集 profile;profile 作为部署
  artifact 与 bundle 一同版本化固定;
- AOT 编译器经 API 接受 `--pgo-profile`(冻结 CLI 不动);profile 校验
  schema + build id + 版本,不匹配 → fail-closed 中止;
- **无 profile 输入 → 输出与今日 kPassAll 逐字节相同**(构造性:全部
  PGO 代码路径由 profile 缺席短路,此保证进 G2);
- 确定性契约:(source, profile, build) → 字节唯一。

### 4.3 发射策略(直接回答姊妹方案 review 的"发射策略缺失")

Lane 2 的 site 发射条件 = **未获 Lane 1 证明** 且 profile 热(命中数
≥ 阈值)且类稳定率 ≥90%。发射形态是**带 guard 的静态 adaptive
opcode**:guard 是字节码里的 tag/shape 检查(miss → generic 慢路径),
**没有 side table、没有 GC root、没有 quicken score、没有运行时
改写** —— 上游 IC 的内存与维护面全部不存在,guard 语义与 IC 同构。
冷 site 一律保持原 opcode:**冷代码零税**。"emitted-cold"(已发射但
profile 证明从不命中的 site)作为独立状态进 §8.3 矩阵,门 <1%。

## 5. Lane 3:运行时 quickening(最后手段)

仅当 Lane 1+2 全部落地、重采样后仍有 ≥2% tick 覆盖且命中率 ≥90% 的
残余热点,才启用姊妹方案 §5 的 quickening/side table/score 设计,
并追加两条(本方案 review 结论):

1. `quicken_score` 属于序列化 struct 的 bitfield —— writer 强制清零、
   reader 忽略,并加"score 非零的运行后重序列化 == 未执行字节"的
   定向测试(§6.3 #3 扩展);
2. OFF/ADAPTIVE 在 test262-fast 上的可观测等价显式入门禁。

## 6. 格式与 opcode 空间(与姊妹方案共享,一处修正)

BC27 + OP_ext(=252,prefix+ext_id+payload)+ 253/254 direct slot +
255 永久非法的混合布局、ExtOpInfo 表、secondary dispatch、reader
canonical-only、writer canonicalize、静态断言组 —— 整体采用姊妹方案
§4 设计,不重复叙述。两处本方案的修正:

1. **direct slot 晋升评估时点**:在 Lane 1 静态发射落地之后重新计算
   剩余份额(静态发射吃掉的热点不再计入 ≥5% 覆盖门槛);Lane 1 的
   无 guard 形态 handler 最短、二级 dispatch 相对占比最大,是 253/254
   的第一候选;
2. **上游版本冲突**:quickjs-ng 上游未来自升 BC_VERSION 可能与 27
   冲突 —— 升级流程必须 re-baseline + 静态断言兜底;capsid 兼容性 ID
   本就隔离跨 vendor 字节码,不依赖裸版本号。

## 7. 管线集成与确定性

```text
P0 解析 → P1 解码/CFG → fixpoint(≤16 轮,每轮:
  P11/P14 → P17 类型传播 → P18 专门化发射 → P16 → P2 → P3.1,P6 重收缩)
→ P7 pc2line → P8 splice/checksum → 验证器
```

- P17 格有限、值只升不降 → 单调收敛;P18 只做替换(1 opcode → 1
  opcode,指令数不增) → 固定点终止性不变;
- 每个 ext opcode 在现有 Insn 分类模型里定义 pop/push/read/write/
  barrier;未分类 ext → barrier(既有 pass 不得穿过未知副作用);
- 验证器扩展:ext 解码、跳转目标不落入 payload、栈高不变量、P9 的
  φ 一致性设计复用;
- 专门化 opcode 是栈操作无操作数(ext 形态 2 字节,direct 形态 1 字节),
  P6 只处理其非栈操作数(若有),新 opcode 本身不参与立即数重收缩;
- 报告仅 stderr,逐 pass 行扩 P17/P18;fail-closed、确定性红线不变。

## 8. 门禁与测量协议

### 8.1 G1 正确性(硬门)

quickjs jscheck/ctest/cxxtest/test262-fast → 每候选 full test262;
computed-goto 与 switch 两种 dispatch 构建;JS_NAN_BOXING=0/1;
Linux ASan+UBSan;capsid RED round-trip、differential(全 fixtures
优化 vs 源码 worker)、fuzz 40k(语料增类型密集与对象字面量);
OFF/ADAPTIVE 可观测等价(Lane 3 若启用)。

### 8.2 G2 零回归(硬门)

PGO-off 与今日 kPassAll 输出逐字节一致(构造性 + 8 个 no-trigger
fixture sha256 门);emitted-cold <1%;任何确认回退 → 修复或 trim。

### 8.3 G3 有效性(go/no-go,配对协议固化)

1 warmup + ≥7 配对交错轮(A/B 逐轮交替,taskset 固定核);结论取
**配对比值的几何均值 + 同号计数 ≥5/7**;跨轮中位数比不构成证据
(§1.2 教训);首测异常必须复测(sablejs 6-round recheck 先例)。
门槛:全性能语料几何均值 ≥2%,至少两个 lane ≥5%,任何 lane 回退
>2% 交错复测确认后 trim。状态矩阵:baseline / Lane1 / Lane2 /
emitted-cold / Lane1+2 / (条件)direct 晋升 / (条件)Lane3。

### 8.4 G4/G5 + 部署门

- G4:逐候选归因(P17 单独、P18 按 opcode 族、guard 形态、direct
  slot),<1% → trim,矩阵归档;
- G5:ophist 前后、dispatch 地板、profile 覆盖率 vs 收益相关图;
- **部署门(新增)**:引擎达标 ≠ capsid 升级格式。26-bundle 语料与
  Hono 请求 mix 逐项无 >2% 回退(对应上游 typescript/babel 教训),
  flag-day 成本清单(host/compiler/worker/既有 trusted bundle 锁步)
  写入决策记录,才允许生产 identity 升 BC27。

## 9. 实施顺序与提交边界

1. **A0 基线**:quickjs commit 固定、原生测试/test262 结果、capsid
   优化器现状审计;**36 个 overlay patch × opcode 表/dispatch/reader
   区域的冲突地图**;
2. **A1 profiling**:共享补丁 + 定向自测 + 采集(quickjs microbench +
   web-tooling 固定 revision + capsid 26-bundle 语料在 worker 变体上);
3. **A2 选择与可证明份额**:四类候选矩阵 + 姊妹方案 §3.4 评分;
   **同时报告 Lane 1 对每个候选的静态可证明份额** —— 第一个
   go/no-go:份额 <15% → Lane 1 降级为传播不发射,直接 Lane 2;
4. **B1 格式**(共享,一次到位):BC27 + OP_ext 基础设施,不发射候选,
   完整 G1;
5. **C1 P17**(纯 AOT,类型格 + 传播 + 屏障测试);
6. **C2 P18**(发射 + golden 字节 + ext 分类);
7. **C3 裁决**:§8 矩阵全跑,keep/trim 写死;
8. **D1(条件)Lane 3**:仅残余热点达标时启用姊妹方案 C 阶段;
9. **D2 收尾**:证据归档(含负结果)、docs/architecture/performance
   同步、opcode/direct slot 占用记录。

每阶段单独 commit,禁止跨阶段混交。

## 10. 风险与强制停止条件

| 风险 | 缓解 |
| --- | --- |
| 静态证明链某条规则错 → 无 guard 错发射 | 保守屏障 + 每规则回归测试 + differential/test262 硬门;T_UNKNOWN 一律不发射 |
| profile 引入非确定性 | profile 版本化 + fail-closed 校验 + (source,profile,build) 字节唯一契约进 G2 |
| 上游 BC_VERSION 冲突 | 升级 re-baseline + 静态断言;兼容性 ID 隔离 |
| 过程间分析面膨胀 | 深度上限 1、SCC 保守、仅 profile 热调用边展开 |
| ext 二级 dispatch 吃掉收益 | direct 晋升按 Lane 1 落地后的剩余份额评估 |
| 新 opcode 破坏既有 pass | 全量分类 + barrier 兜底 + golden 字节测试 |
| 维护成本(上游 IC 第三删除理由) | keep 报告含代码增量账与每 site 状态账;Lane 1/2 天然零状态 |
| 两个 lane 都不达标 | 停止生产接入,提交 profiling 与 no-go 报告;不因 sunk cost 升 BC27 |

## 11. 与姊妹方案的执行编排

两案共享 A0-A2 与 B1,只做一次;之后**先执行本方案 C1-C3** —— Lane 1/2
没有任何运行时状态、GC root、side table 或内存增量,风险面比姊妹方案
C 阶段低一个数量级,且其测量结果直接决定 Lane 3 的残余热点是否值得
quickening。若 A2 显示静态可证明份额 ≥ 热点 30%,姊妹方案 C 阶段推迟;
若 Lane 2 已达标而 Lane 3 残余 <2%,姊妹方案 C 阶段判 no-go 并归档。
