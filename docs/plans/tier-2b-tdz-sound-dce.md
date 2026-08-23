# Tier-2b 技术方案：TDZ 健全的死存储消除（P16）——字节码 AOT 边界内最后一步

> 执行者：其他 AI。本文件是自包含的执行方案；所有代码锚点、字节期望、
> 门槛与判据已预先写死。执行前先通读 docs/bytecode-aot-optimizer.md §11
> （tier-2 裁决背景）与本文件。

## 0. 背景与证据

tier-2（G4）裁掉了 SSI 套件：语料净贡献 2 条指令（0.016%）。但 G5 opcode
直方图（`bench/tools/ophist.py`）定位了一个**未被裁掉的、已量化的**残留：

arith-rt 优化后循环体 76 条指令的真实形状（解码自
`bench/results/exec-throughput-20260823T140906/arith-rt.opt.qjsb`）：

```
pc   0 set_loc_uninitialized; push_0; put_loc0      ← acc=0（活，保留）
pc   5 set_loc_uninitialized; push_0; put_loc1      ← i=0（活，保留）
pc  10 get_loc_check; push_i32; lt; if_false         ← 循环测试
pc  24..69  16 个 set_loc_uninitialized 连排         ← 15+ 个 let 的 TDZ marker（死）
pc  72 push_i16; put_loc2                            ← 死存储
pc  76..178  14 个 (push_i32; put_loc8) 对           ← 死存储
pc 180..223  acc 累加 / i 自增 / goto16 / return      ← 活
```

P2/P3.1 折叠把全部 21 条算术指令变成了 `push_i32`，使 `let x = <常量>` 的
init store 变成死存储；唯一的 slot 读（pc 10/180/194/216）都只读 slot 0/1
（acc/i）。**可移除集 ≈ 46 条（16 marker + 15 push + 15 put），占循环体
60%，每条执行 300k 次/请求**。动态指令执行份额 ≫ G4 的 1% 动态门槛。

被裁的 P12'（commit 4465f36，git 历史可查）之所以 0 触发，是因为它的 TDZ
保守护栏是"任何有 marker 写的槽整体排除"（
`git show 4465f36:tools/bytecode_optimize.cc` 第 2755-2795 行的 `tdz_slot`）。
本 pass 用精确的槽值活性替换该护栏。

**先验期望（只做对照，不做承诺）**：arith-rt opt-vs-raw 从 +40.31% →
+55%~70%（~15ms/请求，按 ~1.15ns/dispatch 粗算）；cascade-rt 小涨；
prop-loop/prop-hoist 小涨；其余 fixture 字节不变。语料静态归因预计
60-80 条 / 12,645 ≈ 0.5-0.6%——**低于 G4 静态 ≥1% 门槛，本 pass 按 G4
"纯动态收益类"判据（目标 fixture 动态执行份额 ≥1%）裁决**，这是预先登记
的裁决框架，不是事后找补。

## 1. 目标与非目标

**做**：P16 死存储消除（TDZ 健全），集成进固定点，测试+基准+文档。

**不做（红线，任何一条违反即回滚）**：
- 不动 vendor/、patches/、ComputeBuildIdentity.cmake、worker/运行时；
- 不动冻结 CLI（pass 开关仅 API：`PassFlags` 位）；
- 不动 P7 pc2line remap、emitter、verifier 的既有语义——P16 在 P7 之前
  运行，删除指令后 P7 自动重映射，**pc2line 零新增工作**；
- 不新增/重排 cpool 条目；输出保持 BC_VERSION 26；
- fail-closed 不变：任何未知形态 → 编译中止 exit 1，不产出文件；
- 只删指令，绝不新增指令（总指令数单调减 → 固定点收敛性保持）。

## 2. 理论依据（TDZ 健全性论证）

quickjs-ng 中 `set_loc_uninitialized s` 就是向槽 s 写入特殊值
JS_UNINITIALIZED（marker 即槽值，不是位掩码）；`put_loc` 覆盖它；
`get_loc_check` 读到 marker 时 throw。因此：

- **纯槽值活性即可证健全**。对非检查型 store（put_loc/put_loc8/put_loc0-3，
  不含 put_loc_check——check 形式在 marker 在场时可能 throw，删除会改变可
  观察异常，v1 不做）：
  - store 可删 ⟺ 槽 s 在 store 之后、下一次写入之前，任何路径上无读。
  - marker 可删 ⟺ 槽 s 在 marker 之后、下一次写入之前，任何路径上无读。
  - 二者同时成立 → marker + 值生产者 + store 整体可删：槽值此后不可观察。
- 反向（旧护栏担心的"marker 留在原位导致 check 提前 throw"）在本规则下
  不可能：有读 → 活性保留 → 不动。
- 循环携带活性自动覆盖：backedge 使下一轮迭代的读成为 live_out 的一部分
  （arith 的 acc/i 因此保留）。
- captured 槽（`FuncRecord::captured`，vardefs is_captured 位）任何时刻
  可能被闭包读 → 整体排除。
- 值生产者：仅当 store 前一条指令是无副作用纯 push（分类见 §5.2）时才随
  store 删除；否则（如 call 结果）整个序列原样保留——保守，不引入 drop
  变体。net 栈效应：marker(0) + push(+1) + put(−1) = 0。

## 3. 代码锚点（当前工作树，commit c391368）

| 位置 | 内容 |
| --- | --- |
| `tools/bytecode_optimize.cc:805-816` | `struct Insn`（op/old_off/target/imm/aux） |
| `tools/bytecode_optimize.cc:1125-1146` | `is_get_loc_op`/`is_put_loc_op`/`is_set_loc_op`/`is_slot_mut_op` |
| `tools/bytecode_optimize.cc:1148-1161` | `is_slot_alias_barrier`（eval/with/fclosure/using_dispose） |
| `tools/bytecode_optimize.cc:818-821` | `is_small_int_push` |
| `tools/bytecode_optimize.cc:1948-1999` | `apply_crossbb` 的 CFG 构造模式（leaders + block_id + bstart/bend）——P16 复用同一模式 |
| `tools/bytecode_optimize.cc:1035-1045` | `struct RewriteStats` |
| `tools/bytecode_optimize.cc:2749-2779` | `rewrite_function` 固定点循环（16 轮上限） |
| `tools/bytecode_optimize.cc:2966,3003` | report fprintf 两处（缩进不同，逐个 Edit） |
| `tools/bytecode_optimize.h:42-54` | `enum PassFlags`（kPassAll = P2|P31|P11|P14） |
| `tests/test_bytecode_optimizer.cc:184-232` | 测试 harness（Builder + expect_code mask 0xffffffffu） |
| `tests/test_bytecode_optimizer.cc` | p2-crossbb golden（当前期望 `bb cf bc 28`） |

参考（只读）：`git show 4465f36:tools/bytecode_optimize.cc` 第 2755-2810 行
——归档版 P12' 的 store-removal 条件与 TDZ 护栏（新 pass 不需要 SSI 形式，
但可对照其 opcode 分类）。

## 4. 实现步骤

1. **PassFlags**（`tools/bytecode_optimize.h`）：`kPassP16 = 1u << 4`；
   `kPassAll` 加 P16；更新头部 G4 注释（"P12' 的 TDZ 健全版以 P16 回归，
   直改层"）。
2. **新函数** `apply_dead_store_p16(std::vector<Insn>* insns, std::vector<uint8_t>* dead, const std::vector<uint8_t>& captured, RewriteStats* stats)`（放
   `apply_tier2_direct` 之后）：
   a. 门控：函数体含任何 `is_slot_alias_barrier` op、`OP_arguments`、
      `OP_get_arg`/`OP_get_arg0`/`OP_get_arg1`/`OP_get_arg2`、
      `OP_apply_eval`、`OP_with_jump` → 整个函数跳过（return false）。
   b. 按 `apply_crossbb` 的模式建 CFG（leaders/block_id/bstart/bend）。
   c. 逐块反向扫描做**标准槽值活性**（backward worklist）：
      - 读（live_in += s）：`is_get_loc_op`（含 get_loc_check）、
        `is_slot_mut_op`（inc_loc/dec_loc/add_loc/post_inc/post_dec/close_loc
        ——读+写，写方向也 kill）、get_arg 族已在门控排除；
      - 写（live_in −= s）：`is_put_loc_op`、`is_set_loc_op`、
        `OP_set_loc_uninitialized`；
      - var-ref op（get_var_ref*/put_var_ref*）只触碰 captured 槽
        （闭包槽被排除）→ 忽略，但保险起见把
        `OP_get_var`/`OP_put_var`/`OP_get_var_check` 视为读全部 captured 槽
        （对非 captured 槽无影响）。
   d. 单遍删除（用原始 insns 上算好的 live_out，不迭代）：
      - 对每条 `is_put_loc_op` 且 **非 check**（put_loc/put_loc8/put_loc0-3）
        的 store s：`s ∉ live_out(store)` 且 `captured[s]` 假 且 前一条指令
        是无副作用纯 push（§5.2 分类）→ 删 push + store，`stats->folds_p16 += 2`；
      - 对每条 `OP_set_loc_uninitialized s`：`s ∉ live_out(marker)` 且
        `captured[s]` 假 → 删 marker，`stats->folds_p16 += 1`。
      - 两条规则相互独立：marker 组与 store 分离的 arith 形状（marker 连排
        + push/put 对）自然覆盖；有中间读则只保留 marker。
   e. 返回是否删除了任何指令。
3. **RewriteStats**：加 `uint64_t folds_p16 = 0;`。
4. **固定点集成**（`rewrite_function` 循环内，`apply_tier2_direct` 之后）：
   ```cpp
   if ((passes & kPassP16) &&
       apply_dead_store_p16(&insns, &dead, f.captured, stats)) {
       round_changed = true;
   }
   ```
   P16 只删指令不创造新折叠机会，理论上固定点仍 ≤16 轮收敛；不收敛
   保护保持原样。
5. **Report**：三处 fprintf（2966 行附近两处 12 空格缩进 + 3003 行一处
   8 空格缩进）格式扩为
   `"... folds P2 %llu P3.1 %llu P11 %llu P14 %llu P16 %llu, shrinks %llu\n"`，
   args 加 `stats->folds_p16`。三处**逐处 Edit，勿 replace_all**（缩进不同）。
6. **analyze**（bench/bin/analyze.cc 若统计 foldable）不需要动——P16 是
   动态类 pass，静态 analyze 天花板不覆盖它；保持现状。

## 5. 精确分类

### 5.1 读（live_in += s）
`is_get_loc_op`（get_loc/get_loc8/get_loc_check/get_loc0-3）＋
`is_slot_mut_op` 现有成员（inc_loc/dec_loc/add_loc/set_loc_uninitialized/
close_loc）中除 set_loc_uninitialized 外的全部＋
`OP_post_inc`/`OP_post_dec`/`OP_mul_loc`/`OP_div_loc`/`OP_mod_loc`/
`OP_and_loc`/`OP_or_loc`/`OP_xor_loc`/`OP_shl_loc`/`OP_shr_loc`/`OP_sar_loc`/
`OP_not_loc`/`OP_pow_loc`/`OP_plus_loc`——**执行前用
`grep -n 'DEF(.*_loc' vendor/txiki.js/deps/quickjs/quickjs-opcode.h` 核对
quickjs-ng 实际存在的全部 `*_loc` in-place 算术 op，逐一列入读+写**
（注意 `is_slot_mut_op` 现仅列 inc/dec/add，是历史简化，勿照抄）。
另加 `OP_dup_loc`/`OP_swap_loc` 等若存在。写方向同一 op kill。

### 5.2 纯 push（可随 store 删除的值生产者）
`is_small_int_push`（push_minus1..push_7、push_i8/16/32）＋
`OP_push_const`/`OP_push_atom_value`/`OP_undefined`/`OP_null`/
`OP_push_false`/`OP_push_true`。**不含** push_this（可能读 this 槽）、
不含任何从栈/槽取值的 op（dup、get_loc…）。

### 5.3 写（kill s）
`is_put_loc_op`＋`is_set_loc_op`＋`OP_set_loc_uninitialized`。store 删除
仅限非 check 的 put_loc 族（put_loc/put_loc8/put_loc0-3）。

### 5.4 槽数
`nslot = f.stack_size`；`captured` 向量长度 = vardef 数（≤ var_count），
访问前判界（`s < captured.size()`）。

## 6. 测试

### 6.1 Golden 更新（tests/test_bytecode_optimizer.cc）
- **p2-crossbb**：P16 现在会删除死存储 `x=1`（无 marker 形状：
  push_1; put_loc0 前无 marker）→ golden 从 `bb cf bc 28` 改回 **`bc 28`**，
  注释改为：`// P16 (TDZ-sound dead-store elimination, tier-2b) removes the
  dead store x=1: slot 0 is never read after the store on any path, the
  producer is the pure push_1. Output is push_2; return.`

### 6.2 新增 Golden（用现有 Builder 构造，逐字节写死期望）
| 用例 | 形状 | 期望 |
| --- | --- | --- |
| dead-triple-marker | marker s; push_i32; put_loc8 s; return（无读） | 三指令全删 |
| dead-store-no-marker | push_1; put_loc0; push_2; return | `bc 28` |
| tdz-keep-read-after | marker s; push_i32; put_loc8 s; get_loc_check s; return | 原样不变 |
| tdz-keep-marker-live | marker s; get_loc_check s; push_i32; put_loc8 s; return（marker 后、store 前有读） | 原样不变（marker 活） |
| loop-carried-keep | marker s; push_0; put_loc8 s; [回边循环] get_loc_check s … | 原样不变 |
| captured-keep | vardef 标 captured；marker s; push_i32; put_loc8 s（无读） | 原样不变 |
| producer-side-effect | marker s; <call 产值>; put_loc8 s（无读） | 原样不变（call 非纯 push） |
| barrier-keep | 函数体含 OP_eval | 原样不变 |

### 6.3 差分与 fuzz
- 差分：全量 `tests/fixtures/*.js` 优化字节码 worker vs 源码 worker，
  响应体逐字节一致（现有差分脚本，照 tier-2 用法）。
- fuzzer：`/tmp/bo_fuzz_driver.cc` + `tests/fuzz/fuzz_bytecode_opt.cc` +
  `libcapsid_bytecode_opt.a`，g++ `-fsanitize=address,undefined`（环境无
  clang++），≥40k runs 干净。
- RED `runtime_bytecode_compiler_round_trip` 原样通过。

## 7. 测量与门槛

```sh
# 发布构建需干净树：所有改动 + 测试先 commit
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure -R 'bytecode|attestation|build_identity|host_managed_trusted_bytecode'
bench/exec-throughput.sh   # 13 fixtures，taskset 0-3，ROUNDS=5 中位数
```

结果目录会生成新 `bench/results/exec-throughput-<ts>/`（gitignored，本地
artifact）。

**预登记判据**：
1. G1：上述测试全绿；差分逐字节；fuzz 干净。否则修复，无讨论。
2. G2：不触发路径字节相同（构造性：只有满足 §4.2d 条件才删）。
   八个此前字节相同（0% ceiling）的 fixture 必须仍与
   20260823T140906 的 opt 输出 sha256 一致（P16 在它们上不触发——
   无折叠即无死存储；若不一致，先查明是否它们本来就含死存储，
   差分通过才接受，并在报告中单列）。
3. G3 有效性：arith-rt opt-vs-raw 中位数 ≥ +43.31%（现有 +40.31% 之上
   任意正提升即过，但**若提升 < +3 个百分点，按 G4 判据裁掉 P16 并如实
   报告**——预期 +55%~70%）。
4. G4 归因：开关矩阵（kPassAll vs kPassAll 去掉 P16）逐 fixture 报告
   P16 的 insn/bytes 消除；语料静态归因预计 <1%——**本 pass 以动态判据
   裁决**：arith-rt 上被删指令的动态执行份额 ≈46/76×300k（≫1%），以
   ophist 直方图（`bench/tools/ophist.py`）前后对比为证据归档。
5. G5：更新 §11 直方图表（arith-rt opt 76→~30，修正"32 条脚手架"为实测
   值——实际可移除集含 push 生产者，≈44-46 条）。

**Trim 标准**：若 arith-rt 提升 <3 个百分点或任何 fixture 回退 >2%（机器
干扰带，需复测确认非干扰），裁掉 P16（保留 `kPassP16` 位定义与函数，仅
从 kPassAll 移除 + §11 记录），如 v1/tier-2 先例。

## 8. 文档收尾

- `docs/bytecode-aot-optimizer.md` §11：tier-2b 小节——G1-G5 裁决、
  P16 归因数字、直方图更新、trim 或 keep 结论；§5 管线描述加 P16。
- `docs/architecture.md` line 5：管线列表加 P16。
- `docs/performance-benchmarks.md`：新 run 表格 + 静态缩减数。
- `tools/bytecode_optimize.h`：G4 注释同步。
- 全部 commit（消息带 Co-Authored-By: Claude <noreply@anthropic.com>）。

## 9. 风险

| 风险 | 缓解 |
| --- | --- |
| 活性分析 bug → 删活 store | 差分 + fuzz 40k + RED；条件本身是标准向后活性，实现 50 行内 |
| `*_loc` in-place 算术 op 漏分类 | §5.1 强制 grep 全表核对；漏一个 = 潜在误删，列为必检 |
| marker 与 store 跨块（本形状同块但保守处理跨块） | live_out 按 CFG 算，天然覆盖；标记活则保留 |
| put_loc_check 语义（可能 throw） | v1 明确排除 check 形式，golden 覆盖 |
| 提升不及预期 | 预登记 trim 标准（§7），照 tier-2 先例执行 |
| 固定点不收敛 | 单调删指令，≤16 轮保护原样 |

## 10. 执行顺序

1. PassFlags + stats + report（编译通过，输出含 P16 0）
2. apply_dead_store_p16 实现 + 固定点集成
3. Golden 更新 + 6.2 全部新 golden（先写测试后过测试）
4. commit → release 构建 → ctest → 差分 → fuzz 40k
5. exec-throughput 全量 + P16 开关矩阵
6. G1-G5 裁决 → keep/trim 决定 → §8 文档 → 最终 commit
