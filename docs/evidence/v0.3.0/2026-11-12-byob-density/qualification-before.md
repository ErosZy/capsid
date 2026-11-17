# v0.3.0 当前验收与兼容边界

更新时间：2026-11-11T17:18:56.111989+00:00。**v0.3.0 尚未完成全平台验收。** 当前冻结代码候选为 `a4bbbb575c193e11bab66221cace00aeb2fbf929`，SDK78，5125份源码文件；逐字节核对通过。主工作区 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，没有更新主分支、标签或发布。

本轮[证据索引](evidence/v0.3.0/2026-11-12-darwin-ci-r2/index.json)、[进度记录](evidence/v0.3.0/2026-11-12-darwin-ci-r2/progress.json)和[任务清单快照](evidence/v0.3.0/2026-11-12-darwin-ci-r2/goal-ledger-at-capture.json)保存当前事实。[此前原文](evidence/v0.3.0/2026-11-12-darwin-ci-r2/qualification-before.md)保留旧候选的压力、DNS、资源身份修正及其原始失败，结果继续绑定各自源码。

## 本轮修正与验证

1. **IPv6 authority 的跨平台解析。** Darwin 的原生解析器会接受带接口 zone 的地址，随后规范化时丢掉 zone。Host 现在先验证 IPv6 字面量的字符范围，再调用原生解析器。扩充数字/接口名称/编码 zone 和 IPv4 嵌入形式的拒绝测试，保留失败时输出不变的断言。Linux 生产、ASan/UBSan、TSan及宽松解析器对照共8项通过，Windows原生配置1项通过；两个旧代码对照失败。Windows不注册的类型化Host模型没有计作通过。实际Darwin结果仍待新任务。
2. **固定 fixture 的原始字节。** 原LF策略在混有22份原始CRLF fixture的代码树上引入了dirty状态，真实Git回归中两个旧策略用例失败；现以 `-text` 禁用fixture换行转换，保留原始LF/CRLF混合字节。Windows/Linux、两种core.autocrlf设置均验证实际checkout字节完全一致且工作树干净。支持集各56项、共112项通过，零跳过。原fixture内容、数据集和哈希断言保持。
3. **Darwin workerd参考锁。** 增加官方同一发布的x64/arm64锁，校验压缩资产与解压二进制SHA及Mach-O架构。执行和安装入口在访问二进制前检查OS/架构，客户端版本和兼容日期/flags保持原锁要求。Windows/Linux各350项参考和互操作控制通过，零失败、零跳过；其中主参考门禁仍保留原296项，新增平台控制在独立支持集中。这不是Darwin实际执行证明。
4. **macOS CI的运行前提。** Homebrew依赖步骤后设置锁定Node24.14.0、Python3.12.3、独立客户端和workerd，CMake绑定Node的规范绝对路径。保存JUnit、原始LastTest和CMakeCache，失败后仍上传。完整矩阵的job预算允许180分钟，单测原预算、原矩阵选择和断言保持。
5. **CI脚本与证据归档。** delegated容器通过bash调用原脚本；workerd证据先完整压缩并生成SHA，再上传，避免平台拒绝内部含 `sha256:` 的路径。真实shell检查确认冒号路径、隐藏文件和二进制字节完整保留，源目录未改。Windows/Linux、LF/CRLF四种表示各240项工作流控制通过，共960项，零跳过。实际修正后的delegated运行和Hosted上传还没有验收结果。

地址解析和CI首批修正相对6df候选改动13份文件，其中生产代码仅 `src/host/config.cc`；不能把该批描述成纯测试修改。后续fixture字节策略和合成producer修正见下方记录。Worker runtime和生产调度保持。[首批候选身份与差异](evidence/v0.3.0/2026-11-12-darwin-ci-r2/candidate.json)可供复核。

## 全量结果与当前运行

| 范围 | 绑定代码 | 当前结果 |
| --- | --- | --- |
| 最新macOS原始全量 | 父候选 `6df7254f…` | 752项：555通过、183失败、14跳过，未通过 |
| 新macOS完整复测 | fixture字节修正祖先96cea的公开代码 `ed8ac0870f5b…` | [实际任务43065514871](https://github.com/ErosZy/capsid/actions/runs/43065514871)已派发，实际构建与矩阵结果待收齐 |
| 本机Windows Release原始全量 | 父候选 `6df7254f…` | 已完成ALL与重复包稳定检查；观察时执行到217/842，完整结果及严格参考验收待结束 |
| 本轮定向控制 | 当前候选 | 配置9项、参考/互操作700项和工作流960项保留各自31b候选绑定；本轮新增checkout支持集112项通过。这些计数不能代替当前全量 |

新macOS任务位于独立验证分支，只使用临时手动入口；checkout精确指向保留完整普通工作流的代码祖先，实际执行原完整macOS矩阵。[派发恢复记录](evidence/v0.3.0/2026-11-12-darwin-ci-r2/hosted-dispatch-recovery.json)保留创建分支前的TLS中断和核对后的恢复，不能将派发当作通过。

最新父候选macOS的[原始失败清单](evidence/v0.3.0/2026-11-12-darwin-ci-r2/previous-macos-failed-and-skipped.json)没有被删减。除本轮修正的地址解析与运行时前提外，还存在：client-disconnect探测的到达时序、receive-buffer前提、channel取消/资源归还、fixture输入关闭、配额测试及原生fetch块形状等问题。必须逐项归因，不能把183项统称为环境失败。

## 仍需完成

- 收齐原始全量、实际macOS和Hosted各构建配置结果；严格参考入口仍拒绝五项既定WPT差异之外的额外失败或不符合契约的跳过。
- 关闭Windows reader-scope那一个强制SIGKILL场景；现有160项记录中该场景失败仍保留。
- 定位fuzz帧测试原60秒预算的超时。原seed私有复放约58.3秒完成10000次，不能覆盖Hosted超时；预分配vector的私有方案仍超时，未合入。不减少10000次、65536最大输入或断言。
- 定位Hosted `benchmark_cold_control` 的PSS稳定性失败，并完成链接后的ABI分配故障检查。R1/R2/R4有定向完成记录；R3/R5/R6/R7/R8组合验收及R9仍未完成。
- 按用户已选方案保留生产调度和单App的N-worker池，改进预热并逐个证明Worker实际收到请求。保留64并发、每Worker冷至少1次/热至少100次、完整后代PSS2048MiB和连续两次相等正读数。正式性能点目前仍为0。
- 四视口是结果页1280/960/820/600宽度的实际布局检查，目前没有截图验收通过。当前不要求用户处理浏览器连接。

## 原始fixture字节后续修正

[最新证据索引](evidence/v0.3.0/2026-11-12-fixture-bytes/index.json)和[checkout原始/修正对照](evidence/v0.3.0/2026-11-12-fixture-bytes/checkout-fixed-controls.json)记录了新策略。当前候选相对31b父候选仅再改 `.gitattributes` 和支持集测试；配置、参考安装器及CI工作流逐字节沿用父候选。先前700项、960项等通过不会重标成新候选完整验收。

此前macOS任务43064163182已经完成锁定Node、Python、独立客户端和原生workerd安装，但Release配置因工作树不干净失败，后续ALL和单测未执行，原干净工作树要求没有放宽。新的任务43065514871增加配置前的实际Git状态记录，并继续原完整macOS矩阵。当前没有宣布Darwin配置或全量通过。

## 父候选Hosted sanitizer终态补充

[原始审校](evidence/v0.3.0/2026-11-12-hosted-sanitizers/terminal-review.json)绑定6df父候选，不属于当前96cea的全量验收。UBSan任务130651697616结论为成功；其1002项实际为997通过、0失败、5跳过，跳过没有计作通过。ASan任务130651696979的普通矩阵1003项为994通过、3失败、6跳过，另两项原始RSS用例均失败，独立内存入口也因普通矩阵存在失败而拒绝。

ASan新增失败是QPS producer二阶段错误记录、参考BYOB bodies上游TLS收到明文HTTP错误、参考BYOB views/brands捕获或清理未完成。断言和原失败保持；QPS测试共享生产session lock存在并发冲突的嫌疑，尚未因果复现，不能仅凭假说修正结果。macOS修正版任务43065514871目前已派发排队，构建与单测结果仍待实际执行。

## 合成producer锁和错误来源修正

[最新候选和证据](evidence/v0.3.0/2026-11-12-producer-locks/index.json)记录了三个测试文件的后续修正。真实文件锁占用在私有目录中重现了原QPS失败：原测试捕获的是无关的 `session-overlap`，没有到达注入的serving失败，随后在缺少failed.json处失败。该机制已复现，但没有精确追踪Hosted原进程调度，原失败不会改写为通过。

三个合成producer测试现在各用自己的临时锁目录，仍执行实际flock；生产全局锁和测量代码没有修改。QPS负向用例要求实际lowLoad→serving调用及 `injected` 错误码。三套测试同时运行共21项通过；真实嵌套重叠拒绝和释放通过，故意移除锁互斥的对照失败，持有无关会话锁时仍到达指定serving注入。没有产生正式性能点，也没有缩小原数据场景。

当前冻结候选在96cea祖先上只增加这三个合成Linux测试文件修正。排队中的macOS任务43065514871仍绑定96cea，不能改标成最新候选的完整验收。BYOB参考TLS和清理失败、原始内存失败、macOS其余原生差异、密度逐Worker预热和正式性能验收继续保持未关闭。
