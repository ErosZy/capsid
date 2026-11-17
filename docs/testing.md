# Testing and Validation

Updated 2026-11-17. This is the current entry point. The maintained commands,
prerequisites and failure handling are in the [testing guide](testing-guide.md),
not in a historical sequence of implementation rounds.

## Current Sources of Truth

- [Current status/data](current-status.md): release input, actual qualification
  runs, standards conflicts and measured snapshot identity.
- [Testing guide](testing-guide.md): local Debug checks, pinned WPT/reference
  setup, sanitizers, required delegated sandboxing and evidence rules.
- [Published beta qualification](v0.3.0-current-qualification.md): completed
  source-bound acceptance and the one disclosed runner-capacity retry.
- [Conformance](conformance.md): 84 selected WPT files and five retained raw
  standard failures under the current v2 compatibility policy.
- [Source build guide](building.md) and [native Windows](windows.md): actual
  platform prerequisites; a Linux/WSL or Wine run is not native Windows evidence.

## Minimum Documentation/Data Check

From the repository root with the locked Node/Python tools:

```sh
python3 tools/refresh-documentation-data.py --check
node tests/audit-current-docs.mjs .
node --test tests/test_documentation_archive.mjs
node tests/audit-testing-validity-workflow.mjs .
python3 tests/test_perf_archive.py
```

These are real consistency/archive/tool checks, not runtime qualification.
Changed commands need execution where feasible. A test listing is not a run;
a checksum is not package acceptance; a metadata-only control is not a live
reference comparison. Use the guide to select the actual behavioral tests.

## Required Failure Handling

Preserve the original exit, complete raw report, exact source/build/platform,
assertions, deadlines and genuine skips. Do not repeat a failed check until it
happens to pass, broaden exclusions, increase budgets or substitute a different
binary/reference. Required compatibility policy can retain known raw WPT
conflicts without turning them into standards passes.

Historical GCC/musl TSan limitations are not instructions to replace the current
hosted Clang/instrumented-provider profile. Ordinary sandbox skips are not
positive isolation evidence; mandatory delegated gates reject Skip 77. Native
macOS/Windows development does not prove a production strict sandbox.

## Historical source and preserved anchors

The original dated checkpoints and their numbers are preserved byte-for-byte in the
[captured document](history/archives/2026-11-17/testing.md.txt). They are not current release instructions.

<details>
<summary>Legacy section links</summary>

<a id="testing-journal-and-legacy-reference"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1) — testing-journal-and-legacy-reference

<a id="testing-and-continuous-gates"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L3) — testing-and-continuous-gates

<a id="capsid-review-phase-107"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L22) — capsid-review-phase-107

<a id="第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L24) — 第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收

<a id="capsid-review-phase-106"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L45) — capsid-review-phase-106

<a id="第106轮2026-11-07私有上下文与未读gzip分支验证进展"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L47) — 第106轮2026-11-07私有上下文与未读gzip分支验证进展

<a id="capsid-review-phase-105"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L61) — capsid-review-phase-105

<a id="第105轮更新2026-11-07"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L63) — 第105轮更新2026-11-07

<a id="capsid-review-phase-104-in-progress"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L73) — capsid-review-phase-104-in-progress

<a id="第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L75) — 第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常

<a id="已合入主树4处c-api测试助手归还响应额度"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L81) — 已合入主树4处c-api测试助手归还响应额度

<a id="新发现快照正文在部分发送后超时取消会污染运行时异常状态"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L101) — 新发现快照正文在部分发送后超时取消会污染运行时异常状态

<a id="私有js-v7仍按原范围保留"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L111) — 私有js-v7仍按原范围保留

<a id="当前差异与继续顺序"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L117) — 当前差异与继续顺序

<a id="capsid-review-phase-103"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L126) — capsid-review-phase-103

<a id="第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L128) — 第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步

<a id="capsid-review-phase-102"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L157) — capsid-review-phase-102

<a id="第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L159) — 第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof

<a id="capsid-review-phase-101"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L190) — capsid-review-phase-101

<a id="第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L192) — 第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复

<a id="capsid-review-phase-100"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L221) — capsid-review-phase-100

<a id="第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L223) — 第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵

<a id="capsid-review-phase-99"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L252) — capsid-review-phase-99

<a id="第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L254) — 第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径

<a id="capsid-review-phase-98"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L281) — capsid-review-phase-98

<a id="第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L283) — 第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历

<a id="capsid-review-phase-97"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L306) — capsid-review-phase-97

<a id="第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L308) — 第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收

<a id="capsid-review-phase-96"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L339) — capsid-review-phase-96

<a id="第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L341) — 第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正

<a id="capsid-review-phase-95"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L353) — capsid-review-phase-95

<a id="capsid-review-phase-94"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L367) — capsid-review-phase-94

<a id="第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L369) — 第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异

<a id="capsid-review-phase-93"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L388) — capsid-review-phase-93

<a id="第-93-轮2026-10-20五栈-qps-控制器与归档已接通"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L390) — 第-93-轮2026-10-20五栈-qps-控制器与归档已接通

<a id="capsid-review-phase-92"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L401) — capsid-review-phase-92

<a id="第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L403) — 第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续

<a id="capsid-review-phase-91"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L412) — capsid-review-phase-91

<a id="第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L413) — 第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续

<a id="capsid-review-phase-90"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L422) — capsid-review-phase-90

<a id="第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L423) — 第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续

<a id="capsid-review-phase-89"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L436) — capsid-review-phase-89

<a id="第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L437) — 第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成

<a id="capsid-review-phase-88"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L448) — capsid-review-phase-88

<a id="第-88-轮状态2026-10-20密度控制与档案生产器已接通"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L449) — 第-88-轮状态2026-10-20密度控制与档案生产器已接通

<a id="capsid-review-phase-87"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L460) — capsid-review-phase-87

<a id="第-87-轮状态2026-10-20修正性能采集与网页证据边界"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L461) — 第-87-轮状态2026-10-20修正性能采集与网页证据边界

<a id="capsid-review-phase-86"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L472) — capsid-review-phase-86

<a id="第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L473) — 第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照

<a id="capsid-review-phase-85"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L484) — capsid-review-phase-85

<a id="第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L485) — 第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环

<a id="capsid-review-phase-84"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L496) — capsid-review-phase-84

<a id="第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L497) — 第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环

<a id="capsid-review-phase-83"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L506) — capsid-review-phase-83

<a id="第-83-轮代理-22-项完整必测门禁与测试覆盖边界2026-10-20"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L507) — 第-83-轮代理-22-项完整必测门禁与测试覆盖边界2026-10-20

<a id="capsid-review-phase-82"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L518) — capsid-review-phase-82

<a id="第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L519) — 第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存

<a id="capsid-review-phase-81"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L530) — capsid-review-phase-81

<a id="第-81-轮状态2026-10-20延后本地关闭缺口已修复"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L531) — 第-81-轮状态2026-10-20延后本地关闭缺口已修复

<a id="capsid-review-phase-80"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L540) — capsid-review-phase-80

<a id="第-80-轮状态2026-10-20异常父连接的等待事件与本地-close"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L541) — 第-80-轮状态2026-10-20异常父连接的等待事件与本地-close

<a id="capsid-review-phase-79"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L548) — capsid-review-phase-79

<a id="第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L549) — 第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁

<a id="capsid-review-phase-78"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L558) — capsid-review-phase-78

<a id="第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L559) — 第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过

<a id="capsid-review-phase-77"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L566) — capsid-review-phase-77

<a id="第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L567) — 第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通

<a id="capsid-review-phase-76"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L576) — capsid-review-phase-76

<a id="第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L577) — 第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归

<a id="capsid-review-phase-75"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L590) — capsid-review-phase-75

<a id="第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L591) — 第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环

<a id="capsid-review-phase-74"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L605) — capsid-review-phase-74

<a id="第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L606) — 第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文

<a id="capsid-review-phase-73"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L615) — capsid-review-phase-73

<a id="第-73-轮当前进度2026-10-19fetch-调用者上下文恢复"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L616) — 第-73-轮当前进度2026-10-19fetch-调用者上下文恢复

<a id="capsid-review-phase-72"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L625) — capsid-review-phase-72

<a id="第-72-轮当前进度2026-10-19fetch-资源回收与取消语义"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L626) — 第-72-轮当前进度2026-10-19fetch-资源回收与取消语义

<a id="capsid-review-phase-71"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L635) — capsid-review-phase-71

<a id="第-71-轮当前进度2026-10-19跨上下文-promise"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L636) — 第-71-轮当前进度2026-10-19跨上下文-promise

<a id="capsid-review-phase-70"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L645) — capsid-review-phase-70

<a id="第-70-轮当前进度2026-10-19异步回调与执行预算"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L646) — 第-70-轮当前进度2026-10-19异步回调与执行预算

<a id="capsid-review-phase-69"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L655) — capsid-review-phase-69

<a id="第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L656) — 第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期

<a id="capsid-review-phase-68"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L689) — capsid-review-phase-68

<a id="第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L690) — 第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换

<a id="capsid-review-phase-67"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L723) — capsid-review-phase-67

<a id="第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L724) — 第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断

<a id="capsid-review-phase-66"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L757) — capsid-review-phase-66

<a id="第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L758) — 第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io

<a id="capsid-review-phase-65"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L790) — capsid-review-phase-65

<a id="第-65-轮当前进度2026-10-19异常快照时序内置函数与严格回归"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L791) — 第-65-轮当前进度2026-10-19异常快照时序内置函数与严格回归

<a id="capsid-review-phase-64"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L829) — capsid-review-phase-64

<a id="第-64-轮当前进度2026-10-19windows-原生参考与完整矩阵关闭及错误事件流程"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L830) — 第-64-轮当前进度2026-10-19windows-原生参考与完整矩阵关闭及错误事件流程

<a id="capsid-review-phase-63"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L868) — capsid-review-phase-63

<a id="第-63-轮当前进度2026-10-18sdk-正式修正与-c17c18-原始帧回归"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L869) — 第-63-轮当前进度2026-10-18sdk-正式修正与-c17c18-原始帧回归

<a id="capsid-review-phase-62"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L899) — capsid-review-phase-62

<a id="第-62-轮当前进度2026-10-18引擎修复全量验证与-c17c18-线路反例"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L900) — 第-62-轮当前进度2026-10-18引擎修复全量验证与-c17c18-线路反例

<a id="capsid-review-phase-61"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L931) — capsid-review-phase-61

<a id="第-61-轮当前进度2026-10-18并发失败诊断与-hono-冷启动耗时"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L932) — 第-61-轮当前进度2026-10-18并发失败诊断与-hono-冷启动耗时

<a id="第-60-轮当前进度2026-10-18出站-ack-压力前提与真实-worker-夹具"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L960) — 第-60-轮当前进度2026-10-18出站-ack-压力前提与真实-worker-夹具

<a id="第-59-轮当前结论2026-10-18"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1003) — 第-59-轮当前结论2026-10-18

<a id="第-57-轮验收2026-10-17quarantine-顺序代际校验与进程容量"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1011) — 第-57-轮验收2026-10-17quarantine-顺序代际校验与进程容量

<a id="第-56-轮验收2026-10-17managed-进程观察与替换回调生命周期审校"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1047) — 第-56-轮验收2026-10-17managed-进程观察与替换回调生命周期审校

<a id="第-55-轮验收2026-10-17managed-生命周期响应与实际-os-退出审校"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1064) — 第-55-轮验收2026-10-17managed-生命周期响应与实际-os-退出审校

<a id="第-54-轮验收2026-10-17workerd-wpt-profile-精确映射与证据核验"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1080) — 第-54-轮验收2026-10-17workerd-wpt-profile-精确映射与证据核验

<a id="第-53-轮验收2026-10-17managed-预热-worker-接管失败回收"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1098) — 第-53-轮验收2026-10-17managed-预热-worker-接管失败回收

<a id="第-52-轮验收2026-10-17host-drain-信号等待与真实退出状态"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1118) — 第-52-轮验收2026-10-17host-drain-信号等待与真实退出状态

<a id="testing-layers"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1171) — testing-layers

<a id="validity-rules"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1182) — validity-rules

<a id="common-commands"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1199) — common-commands

<a id="required-workerd-websocket-comparison"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1239) — required-workerd-websocket-comparison

<a id="egress-and-tls-targeted-regression"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1368) — egress-and-tls-targeted-regression

<a id="tsan-m1c-gate"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1397) — tsan-m1c-gate

<a id="platform-contract-gates"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1424) — platform-contract-gates

<a id="environment-based-sandbox-evidence"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1436) — environment-based-sandbox-evidence

<a id="how-current-evidence-is-obtained"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1483) — how-current-evidence-is-obtained

<a id="2026-10-16-本地-gcc-tsan-生命周期专项"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1522) — 2026-10-16-本地-gcc-tsan-生命周期专项

<a id="2026-10-16-external-accept-与-worker-退出专项更新"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1538) — 2026-10-16-external-accept-与-worker-退出专项更新

<a id="2026-10-16-worker-退出后的连接回收专项"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1556) — 2026-10-16-worker-退出后的连接回收专项

<a id="2026-10-16-adoption-提交与关闭的专项"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1572) — 2026-10-16-adoption-提交与关闭的专项

<a id="2026-10-16-pending-handshake-fetch-取消"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1590) — 2026-10-16-pending-handshake-fetch-取消

<a id="2026-10-16-完整池-pending-accept-故障分派"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1599) — 2026-10-16-完整池-pending-accept-故障分派

<a id="2026-10-17-外部移交的-drain-窗口专项审校第-48-节"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1609) — 2026-10-17-外部移交的-drain-窗口专项审校第-48-节

<a id="2026-10-17-本地-503-晚到回调与池关闭专项审校第-49-节"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1625) — 2026-10-17-本地-503-晚到回调与池关闭专项审校第-49-节

<a id="2026-10-17-写超时真实性和完整池背压审校第-50-节"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1641) — 2026-10-17-写超时真实性和完整池背压审校第-50-节

<a id="第-58-轮本地验证更新2026-10-17"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1654) — 第-58-轮本地验证更新2026-10-17

<a id="linux-production-and-sanitizer-sandbox-profiles"></a>
[Historical section](history/archives/2026-11-17/testing.md.txt#L1668) — linux-production-and-sanitizer-sandbox-profiles

</details>
