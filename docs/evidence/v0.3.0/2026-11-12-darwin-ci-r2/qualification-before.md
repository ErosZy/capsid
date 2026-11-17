# v0.3.0 当前验收与兼容边界

更新时间：2026-11-11T16:29:46.358932+00:00。v0.3.0 尚未封板。当前代码候选为 `6df7254fc1bb5a3e19a1083dc9c5831431459e29`，SDK78，5123份源码文件；代码逐字节核对通过，本文随后更新。主分支 HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未更新主分支、标签或发布。

当前入口为[本轮证据索引](evidence/v0.3.0/2026-11-12-pressure-dns/index.json)和[任务清单快照](evidence/v0.3.0/2026-11-12-pressure-dns/goal-ledger-at-capture.json)。此前记录保存在[原文归档](evidence/v0.3.0/2026-11-12-pressure-dns/qualification-before.md)，旧测试结果继续绑定原源码，不能据此宣布新候选通过。

## 本轮已完成的修正

1. **队列压力测试协调。** 原流程等待16个 producer 的首窗口后再提交 subject 和剩余 credit，Host 的准备停顿会消耗已运行 producer 的300ms期限。私有对照保留原生产对象，400ms停顿可以触发缺少压力的失败。修正后将16个 producer 和 subject 一次提交，通过128KiB初始窗口提供足够credit，并从首个真实producer输出开始hold。原64请求用例保留。
   [Windows实际检查](evidence/v0.3.0/2026-11-12-pressure-dns/windows-pressure-and-dns.json)与[Linux三种配置](evidence/v0.3.0/2026-11-12-pressure-dns/linux-pressure-profiles.json)均通过：仍要求300ms请求期限、1500ms writer hold、实际1MiB占用、终止先阻塞后恰好发送一次、30秒CTest预算。credit不足和取消hold两项[负向对照](evidence/v0.3.0/2026-11-12-pressure-dns/pressure-rejected-insufficient-credit.log)均失败。旧全量的50008字节观测及额外失败保留，不能回写为通过，也没有声称精确追溯关闭那一轮的全部原因。
2. **受控DNS测试。** 原用例在带搜索域的实际进程环境中复现失败，现使用带根点的完整名称，生产resolver策略未改。[Linux检查](evidence/v0.3.0/2026-11-12-pressure-dns/linux-dns-search-regression.json)12组，加Windows6组，共18组通过；真实A/AAAA、NXDOMAIN、64次取消及资源断言保留。CTest固定本进程的搜索域和ndots，注册审核拒绝缺失、弱化和重复覆盖；[Windows/Linux各605项控制](evidence/v0.3.0/2026-11-12-pressure-dns/dns-registration-controls.json)通过，零跳过。这些检查不替代macOS原生结果。
3. **资源身份测试前提。** 完整GCC UBSan worker在第三次部署的128MiB地址空间限制下无法初始化VM。私有对照使用同一个worker二进制，将该成功部署场景调整为256/192MiB两个有限值后，四代身份差异检查及原资源关系、低地址空间限制检查通过。[原生证据](evidence/v0.3.0/2026-11-12-pressure-dns/ubsan-identity-limit-proof.json)保留原失败。诊断链接的Host第三方provider未插桩，因此不宣称它是完整UBSan矩阵验收；新版Hosted结果仍必需。

本轮改了七个测试／构建文件，生产runtime和调度没有改变。[源码候选记录](evidence/v0.3.0/2026-11-12-pressure-dns/candidate.json)保存完整差异和身份。

## 正在执行与新增未关闭项

- [新的完整Hosted工作流](https://github.com/ErosZy/capsid/actions/runs/43057754558)运行在独立分支 `codex/v030-hosted-validation-r4`，提交 `6da9f5de7f98573b71f3b6c769cbcd98f457925f` 的树与当前私有代码候选相同。[分支与派发证据](evidence/v0.3.0/2026-11-12-pressure-dns/hosted-dispatch.json)不代表测试通过。
- 新源码已完成Windows干净checkout、9个固定子模块和依赖锁核对；新的独立Release ALL构建曾在末段遇到文件占用错误，原日志保留；确认没有相关测试或链接进程在运行后，一次受控恢复通过完整ALL，两次打包的二进制保持稳定。新候选842项未过滤全量已经实际启动，严格workerd平台参考验收仍待该轮完成。此前b284源码的842项原全量仍为828通过、6失败、8跳过，其中pressure额外失败，原严格入口拒绝该轮。
- [前轮macOS实际全量](https://github.com/ErosZy/capsid/actions/runs/43049957600)已完成ALL，但752项中554通过、184失败、14跳过。[失败与跳过清单](evidence/v0.3.0/2026-11-12-pressure-dns/macos-failed-and-skipped-cases.json)保留。实际使用Node22而锁定要求24.14.0，缺少参考运行时；同时存在原生失败，例如两个配置测试在Darwin接受了带zone的IPv6 authority，以及socket探测、receive-buffer前提和channel资源归还。必须分别诊断，不能统一标为环境失败；本轮尚未修正这些macOS问题。
- 最新Hosted fuzz完成ALL及8项测试，`fuzz-websocket-frames_smoke`在原60秒预算下超时，seed为876915245，日志持续推进至至少9289次，但缺少10000 DONE。应按该seed重放并检查执行成本；不减少输入、缩短payload或改成超时也算成功。此前独立Hosted9/9和七个fuzzer共70000次完整记录仍保留各自身份，不能覆盖这次失败。

## 后续原始结果补充

[后续终态审查](evidence/v0.3.0/2026-11-12-pressure-dns/hosted-terminal-followup.json)新增以下事实，未改写此前时点：

- fuzz原样seed重放在原60秒预算内完成10000次，耗时约58.3秒；它不覆盖Hosted原超时。仅在私有参考帧构建器中预分配vector的对照仍超时，[两次完整记录](evidence/v0.3.0/2026-11-12-pressure-dns/fuzz-fixed-seed-and-private-proposal.json)保留，该方案未合入。相同seed在不同插桩二进制中也不保证相同的变异序列，不能据此声称获得了确定的性能改善。
- 前轮Hosted Release原1098项有7项失败：五项既定WPT差异，以及 `host_dns_reserved_invalid`、`benchmark_cold_control`。后者实际报PSS未在原次数内稳定，尚待定位；严格参考入口拒绝额外失败。后续delegated容器在脚本执行权限处退出126，证据上传又因保留目录中 `sha256:` 的冒号被平台拒绝；两项CI接线修正尚未合入，不能宣称相应测试已运行或证据已上传。
- 前轮Hosted Windows已通过ALL，随后workerd gate返回失败。[原始JUnit审查](evidence/v0.3.0/2026-11-12-pressure-dns/hosted-windows-original-junit-review.json)确认134项中132通过、2失败、零跳过：固定参考fixture的SHA差异精确对应LF／CRLF转换；另一个reader-scope场景在160次捕获中的 `native|prototype|default|foreign_release_unread` 被强制SIGKILL，原因未关闭。哈希断言和原数据集未修改，换行修正尚未合入。这些结果不属于本机842项原全量，也不属于正在执行的新源码全量。

## 后续验收

先完成新源码本机与Hosted原始结果审查、macOS必要修复、fuzz预算问题定位及链接后的ABI分配故障检查。R1/R2/R4已有定向要求的完成记录；R3/R5/R6/R7/R8仍需组合验收，R9未完成。

用户已确认保留生产调度和原单App的N-worker池。逐worker预热和实际触达证明继续改进；64并发、冷至少1次／热至少100次、完整后代PSS2048MiB及连续两次相等正读数保持。正式性能点仍为0。四视口验收是检查结果页在1280/960/820/600宽度下的布局，目前没有实际截图通过；当前不要求用户处理浏览器连接。

[最新本机启动与Windows gate证据](evidence/v0.3.0/2026-11-12-pressure-dns/native-full-start-and-gate-review.json)记录了构建恢复、实际全量启动、18个相对旧核心的文件差异及仍需修复的两项原gate失败。
