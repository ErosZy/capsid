# v0.3.0 当前验收与兼容边界

最近更新：2026-11-11T16:00:10.661290+00:00。目标继续执行，最新私有源码为 `ab0684ecc570b8409760232b0e67342072d7c820`（0.3.0候选、SDK78、5123份文件）。本次[终态证据索引](evidence/v0.3.0/2026-11-11-windows-release-terminal/index.json)新增Windows完整结果及额外失败，下面保留此前所有修正与验收时点。

## Windows Release原全量已结束，仍未验收

旧核心 `b2848336f5591cf2f5e67799d0df87169022631a` 的[完整842项原始JUnit](evidence/v0.3.0/2026-11-11-windows-release-terminal/ordinary-full-junit.xml)为 **828通过、6失败、8平台跳过，退出8**。除五项已声明的workerd/标准差异外，新增失败 `worker_response_timeout_queue_pressure`。严格平台参考入口按预期拒绝该额外失败，后续参考测试没有继续执行；没有将本轮重新标为成功。

失败轮的fixture报告：超时终止尝试处已观测队列占用50008字节，`blockedTerminals=0`、`terminalHeldMs=0`。这不是连续的全局队列占用记录，不能据此断言队列此前从未满过。[三次原30秒预算单独回放](evidence/v0.3.0/2026-11-11-windows-release-terminal/three-original-budget-replays.json)均通过，观察到1MiB占用、真实终止阻塞及约1.19秒延期；原源码和二进制未改。这些回放不覆盖全量中的失败。

[私有时序观测](evidence/v0.3.0/2026-11-11-windows-release-terminal/private-timing-grid.json)只增加实际终止事件日志和受控的Host准备间隙，复用原生产对象；300ms请求期限、1500ms writer hold、1MiB队列和30秒外层预算均保留。[临界区间对照](evidence/v0.3.0/2026-11-11-windows-release-terminal/private-near-deadline-grid.json)中，250–280ms间隙仍通过，285–300ms能触发同类“没有形成期望终止阻塞”的失败，但没有精确重现原轮50008字节的观测。原始失败的因果定位**尚未关闭**，相关private代码未合入，未重启整套全量，也未放宽断言。

## 已通过与当前继续项

此前[精简Host/worker关闭构建修正](evidence/v0.3.0/2026-11-11-hostoff-fuzz/index.json)已经完成干净ALL、原生9项fuzz及3项JSON/协议边界，并取得[实际Hosted fuzz 9/9成功](https://github.com/ErosZy/capsid/actions/runs/43048552587)。七个fuzzer各10000次的完整终止记录共70000次真实输入独立保存。812项工作流控制和98项workerd支持控制保留各自的源码身份。

[最新macOS复验](https://github.com/ErosZy/capsid/actions/runs/43049957600)已通过ALL构建，正在执行POSIX矩阵；它固定checkout到 `f591d8a40619cba9aaa775db3f49d947ae5673f8`，源码树与最新私有快照相同。[其他Hosted矩阵](https://github.com/ErosZy/capsid/actions/runs/43047138152)继续按实际结果审查。尚不构成最终统一候选的完整验收。

下一步先查清额外pressure失败，补足必要的fixture协调及严格回归，再执行新候选的ABI分配故障扫描和完整原生矩阵。用户已确认保留生产调度，原单App大池的逐worker预热覆盖继续改进；64并发、冷1/热100、全后代PSS2048MiB及稳定双读保留。正式性能点仍为 **0**，四视口布局截图仍待验收，当前不要求用户处理浏览器连接。主分支、标签和发布状态未改，目标保持active。

<details>
<summary>此前验收时点（原样保留，当前状态以上方为准）</summary>

# v0.3.0 当前验收与兼容边界

最近更新：2026-11-11T15:39:09.091336+00:00。当前为 **0.3.0候选、SDK78**，目标保持执行。最新私有源码为 `ab0684ecc570b8409760232b0e67342072d7c820`，5123份文件的代码与Root逐字节一致，文档随后更新。[当前差异核对](evidence/v0.3.0/2026-11-11-hostoff-fuzz/source-delta.json)已包含新增的POSIX测试头文件修正。此次[证据索引](evidence/v0.3.0/2026-11-11-hostoff-fuzz/index.json)记录新的真实通过和仍待完成的部分；下方保留此前原始验收时点。

## 新增修正与实际通过

Hosted原fuzz job在关闭Host和worker时编译失败：FetchRPC的JSON序列测试引用了不存在的 `capsid_jansson` 目标，两个Host专用测试也无条件引用Host库。现在测试构建按需创建实际vendored Jansson，并仅在真实Host库存在时注册这两个Host专用测试；Host开启的测试范围保留。

原缺失依赖已在干净源上复现。修正后的Host/worker关闭配置完成[完整ALL构建、9项fuzz及3项协议/JSON序列测试](evidence/v0.3.0/2026-11-11-hostoff-fuzz/host-off-build-and-gates.json)，全部通过、零跳过。[实际Hosted fuzz复验](https://github.com/ErosZy/capsid/actions/runs/43048552587)也已成功，原始日志确认 **9/9通过**，见[成功日志](evidence/v0.3.0/2026-11-11-hostoff-fuzz/hosted-fuzz-success.log)。其代码提交为 `8bb4ea8943a57209e99bb3a361df912a5ff344bd`，树与私有快照 `f6459acc7827019c305c43f23d9197ccffc28f14` 相同。

第一次原生JUnit只保留每项前1024字节，无法据此声称掌握完整终止计数。因此另用原七个fuzzer二进制、冻结种子的全新副本及原60秒预算执行了[完整原始记录回放](evidence/v0.3.0/2026-11-11-hostoff-fuzz/seven-fuzzer-full-terminal-records.json)：逐个核对 `#10000 DONE` 和最终执行统计，合计 **70000次真实输入**，全部完整日志保留。它属于额外回放，不与原9项CTest混计。

前轮C++11构造修正在实际macOS构建中已越过原失败位置，ALL推进至98%，随后一个adoption测试因间接头文件差异找不到 `kill` 声明而失败。该POSIX分支已[显式包含signal.h](evidence/v0.3.0/2026-11-11-hostoff-fuzz/macos-signal-include.json)，严格原生POSIX编译检查通过，清理逻辑、断言和测试预算未改。[新的完整macOS复验](https://github.com/ErosZy/capsid/actions/runs/43049957600)正在运行，固定checkout到 `f591d8a40619cba9aaa775db3f49d947ae5673f8`；其代码树与最新私有快照相同。尚未得到修正后的macOS完整通过。

## 仍在运行与身份边界

本机旧核心Windows Release全量采集时推进到 **816/842**，后续严格平台参考入口由原队列执行。它绑定 `b2848336f5591cf2f5e67799d0df87169022631a`，不能替代新C++修正候选的全量。[上一轮完整Hosted CI](https://github.com/ErosZy/capsid/actions/runs/43047138152)的镜像修正已实际解决五个Linux job的依赖安装，Linux Release及sanitizer、Windows的后续结果继续审查；原fuzz失败仍保留，由上述独立修正复验补充。

此前Windows/Linux工作流四套控制812项、workerd支持控制98项通过均保留其原身份。最新adoption单个测试头文件增补不计入既有fuzz源码身份，当前仍需链接后的ABI分配故障扫描、最终完整原生矩阵和统一组合验收，R7/R8/R9没有因此关闭。主分支、标签和发布状态未改。

用户已[确认保留生产调度](evidence/v0.3.0/2026-11-11-hostoff-fuzz/density-user-decision-utf8.json)。下一步继续改进原单App大池预热，逐个验证worker触达；64并发、冷1/热100、全后代PSS2048MiB及稳定双读保留，替代App拓扑不作为等价结果。正式性能点仍为 **0**。四视口是检查性能结果页在1280/960/820/600宽度下是否截断或重叠，实际截图尚未验收，当前不要求用户处理浏览器连接。

<details>
<summary>此前验收时点（按原字节保留，最新状态以上方为准）</summary>

# v0.3.0 当前验收与兼容边界

最近更新：2026-11-11T15:21:03.808653+00:00。当前为 **0.3.0候选、SDK78**，整体目标继续执行。最新私有源码快照为 `9f3cca1952b8c3ffc2d2a6b55bdde9d8b5ca04f3`，5123份文件的代码与Root一致；文档随后更新。[本轮证据索引](evidence/v0.3.0/2026-11-11-hosted-findings/index.json)区分已复现的失败、已验证修正和仍在运行的验收。旧核心 `b2848336f5591cf2f5e67799d0df87169022631a` 的完整结果继续保留原身份。

## 本轮真实问题与修正

- macOS实际编译发现C++11的 `vector(count, allocator)` 不可用。`Frame`及三个client缓冲区改用C++11的count/value/allocator构造，保持零元素和可传播的分配异常。[原始libc++失败与修正反例](evidence/v0.3.0/2026-11-11-hosted-findings/libcxx-original-and-corrected.json)已核对；[GCC、Clang/libstdc++、MSVC Debug/Release](evidence/v0.3.0/2026-11-11-hosted-findings/cxx11-native-components.json)及Clang/libc++协议测试和client编译通过。新增加的空帧测试检查默认/值初始化、零字节负载和解析；新源码仍需链接后的ABI分配故障扫描及完整矩阵。
- 首轮Windows实际完成产品编译，随后workerd兼容入口因CRLF无法匹配完整handler块而失败。现在只规范化隔离生成程序的换行；原始fixture字节和摘要计算保持原样。[Windows/Linux各49项完整支持控制](evidence/v0.3.0/2026-11-11-hosted-findings/workerd-crlf-controls.json)通过、零跳过，覆盖全部既有变异作用域及缺失/重复handler块拒绝。冻结的296项控制清单逐字节保留。
- [Hosted网络原始观测](evidence/v0.3.0/2026-11-11-hosted-findings/hosted-network-observation.log)显示三个ubuntu.com域名解析失败，kernel.org Ubuntu镜像的索引返回HTTP200。工作流改用该已观测镜像，保留Ubuntu Signed-By、发行版、组件、严格索引失败及有限重试；镜像由[Linux Kernel Archives](https://mirrors.kernel.org/)提供。此前直接换archive.ubuntu.com的尝试仍为失败，不能称为解决。最新[Windows/Linux、LF/CRLF控制共812项通过](evidence/v0.3.0/2026-11-11-hosted-findings/reachable-mirror-controls.json)，实际完整Hosted依赖安装仍待结果。

macOS runner已从停用的macos-14切到macos-15，Windows/sanitizer job总预算为180分钟，各项CTest自身时限未改。SDK、运行时JS、公开C头文件和生产调度没有因本轮修正变化；原生C++的两份内部源文件确有变化，新候选需要重新完成组合验收。

## 当前实际运行

[最新完整Hosted CI](https://github.com/ErosZy/capsid/actions/runs/43047138152)使用提交 `9dd5c0fb4e46095969f403effcfbecae9b156a7b`，其完整源码树与上述私有快照相同；[jobs时点副本](evidence/v0.3.0/2026-11-11-hosted-findings/actual-latest-jobs.json)记录采集时七个job的运行状态。调度POST已返回204，后续Python只读查询遇到TLS错误；连接器已观察到唯一、精确提交的实际run，未重复调度。本次仍未通过，不代替最终验收。

[单独macOS复验与网络诊断](https://github.com/ErosZy/capsid/actions/runs/43046254783)的macOS job固定checkout到C++修正提交 `c3e62efba93cf5417989a2d1c157f25b56d9c2b7`；采集时正在构建，它不包含后续CRLF/镜像改动，也不代表完整新组合。临时诊断工作流只存在于独立验证分支。

本机Windows普通Release的旧核心完整842项序列仍在执行，采集时完成 **461/842**，控制器/CTest均存活。首轮额外超时、第二轮中断及本轮原预算回放仍保留。该运行结束后按旧核心身份验收，不会把它重新命名成新C++/CRLF候选的全量通过。此前Linux生产1098项、ASan/UBSan1102项、TSan1090项、Windows ASan843项的原始失败及独立兼容入口继续保留，详细计数见下方历史时点。

## 已确认的密度方向与剩余工作

用户已明确选择[保留生产调度、改进预热并逐worker验证触达](evidence/v0.3.0/2026-11-11-hosted-findings/density-user-decision.json)。保留原单App大池、64并发、逐worker冷1/热100、全后代PSS2048MiB和双次相等正值读取；不改并列最低槽调度。此前单worker App替代方案不作为等价结果，其等待队列已在实际探针启动前停止，旧材料保留。原拓扑的预热覆盖实现仍需完成，正式性能点仍为 **0**。

还需审查最新Hosted实际结果、完成新候选的链接后ABI故障扫描和原生完整矩阵、落实上述密度预热并完成同会话正式三车道测量。四视口检查针对性能结果展示页的文字、图表和布局；1280/960/820/600实际截图尚未验收，当前不要求用户处理浏览器连接。R3/R5/R6/R7/R8与最终R9保持未完成；主分支、标签和发布状态未改。

<details>
<summary>此前验收时点（按原字节保留，状态以本页上方为准）</summary>

# v0.3.0 当前验收与兼容边界

最近更新：2026-11-11T14:56:46.880124+00:00。当前为 **0.3.0 候选、SDK78**，整体目标仍在执行。核心输入为 `b2848336f5591cf2f5e67799d0df87169022631a` 的5123份文件；测试可移植性扩展为 `bf79a119265aceea591751a73a543b5a8cb61726`。最新[Hosted 后续运行与证据索引](evidence/v0.3.0/2026-11-11-hosted-followup/index.json)及[首轮审查](evidence/v0.3.0/2026-11-11-hosted-validation/index.json)区分实际运行、失败、已验证修正和仍在排队的工作。

## 实际 Hosted CI 与修正

已推送独立验证分支 `codex/v030-hosted-validation`，并启动了 [Testing validity 实际运行](https://github.com/ErosZy/capsid/actions/runs/43039745761)，提交为 `db29f75da7b7341a5cdb4eba545752cfd654e6a2`，其树与两文件扩展快照相同。主分支未更新，没有新建标签或发布，也没有触发 Release/Pages。

[首次 jobs 时点副本](evidence/v0.3.0/2026-11-11-hosted-validation/first-jobs-at-capture.json)显示：macOS job 未获得 runner、零步骤即取消；Release、ASan、UBSan、TSan 和 fuzz 均在安装依赖阶段因 `azure.archive.ubuntu.com` 解析失败退出100，尚未编译产品或执行测试。首轮 Windows Hosted job 在后续采集时已进入产品编译。这次运行尚未通过，不能计入 Hosted 验收。

已完成三项工作流修正：

- [macOS runner 修正](evidence/v0.3.0/2026-11-11-hosted-validation/macos-runner-repair.json)：由 `macos-14` 改为 `macos-15`，增加退回旧镜像必须失败的控制。GitHub [官方公告](https://github.blog/changelog/2026-10-01-github-actions-macos-14-runner-image-retirement/)确认旧镜像于2026-11-02停用。
- [Ubuntu 依赖修正](evidence/v0.3.0/2026-11-11-hosted-validation/ubuntu-dependency-repair.json)：将 runner 的镜像列表改为 Ubuntu 官方 archive，保留发行版、组件和 Signed-By；为下载设置有限重试和超时，强制 `update --error-on=any`。实际隔离 APT 反例证明，默认更新在下载失败时返回0，严格更新返回100；含义与 [Ubuntu 官方手册](https://manpages.ubuntu.com/manpages/noble/man8/apt-get.8.html)一致。

- [job总时限修正](evidence/v0.3.0/2026-11-11-hosted-followup/job-budget-and-controls.json)：现有完整Windows Release约104分钟、TSan约62分钟，两个Hosted job调整为180分钟总时限以覆盖依赖和构建。各项CTest自身超时不变，零值、过短及过大的job预算均由控制拒绝。

Windows/Linux、LF/CRLF 四套工作流控制共 **812项通过、零跳过**；三个安装脚本通过原生 Bash 语法检查。此前准备脚本的路径/隔离清单错误和测试构造器错误均保留为失败尝试，最终控制来自新目录。修正仅涉及工作流、审计器和其控制测试三份文件；生产运行时、SDK、JS和公开头文件未变。[最新候选](evidence/v0.3.0/2026-11-11-hosted-followup/candidate.json)为 `14d9f21b5970355c73c9e028ee5a9cd7a357a8ea`，已推送到第二独立验证分支 `codex/v030-hosted-validation-r2`，并启动[修正后的完整Hosted CI](https://github.com/ErosZy/capsid/actions/runs/43043160089)。[实际jobs副本](evidence/v0.3.0/2026-11-11-hosted-followup/corrected-jobs-at-capture.json)确认七个job都已运行，macOS已分配 `macos-15` runner；这些是运行中证据，尚未通过。首轮仍在运行的Windows job没有被取消。

## 本机完整矩阵

| 配置 | 原始完整结果 | 后续验收边界 |
| --- | --- | --- |
| Linux生产 | [1098项](evidence/v0.3.0/2026-11-11-final-validation/linux-production-full.json)：1093通过、5失败、0跳过 | 五项锁定workerd/标准差异保留；[严格兼容入口](evidence/v0.3.0/2026-11-11-final-validation/linux-reference-acceptance.json)通过 |
| Linux ASan/UBSan | [1102项](evidence/v0.3.0/2026-11-11-final-validation/linux-asan-full.json)：1095通过、7失败、0跳过 | 五项标准差异及两项原RSS失败保留；[分开的生产RSS与插桩资源验收](evidence/v0.3.0/2026-11-11-final-validation/instrumented-memory.json)通过 |
| Linux TSan恢复全量 | [1090项](evidence/v0.3.0/2026-11-11-final-validation/linux-tsan-recovery-full.json)：1083通过、6失败、1跳过 | 原始差异、RSS失败及不适用探针跳过保留，不宣称TSan执行LSan |
| Windows ASan | [843项](evidence/v0.3.0/2026-11-11-final-validation/windows-asan-full.json)：830通过、5失败、8平台跳过 | [严格平台参考入口](evidence/v0.3.0/2026-11-11-final-validation/windows-reference-acceptance.json)通过；原始普通CTest仍为失败 |
| Windows普通Release | 完整842项正在执行，采集时完成405项，控制器及CTest存活 | [控制器时点副本](evidence/v0.3.0/2026-11-11-hosted-validation/release-controller-at-capture.json)不等于完成；仍需最终JUnit、身份复核及严格平台参考入口 |

Windows Release 首轮额外的参数控制超时及三次原预算单独回放均保留；第二轮中断也保留[452项前缀与原日志](evidence/v0.3.0/2026-11-11-final-validation/release-interruption.json)。当前完整序列中的同一参数控制在原预算内21.89秒通过，没有放宽时限。当前本机矩阵仍绑定原核心身份，新增的CI文件由上述独立控制及后续真实Hosted运行验收。

两文件musl测试扩展已完成[干净ALL与包消费](evidence/v0.3.0/2026-11-11-final-validation/musl-build-package.json)、[严格沙箱](evidence/v0.3.0/2026-11-11-final-validation/musl-strict.json)、[五个实际adoption场景](evidence/v0.3.0/2026-11-11-final-validation/musl-adoption.json)和[fuzz依赖等价复核](evidence/v0.3.0/2026-11-11-final-validation/fuzz-extension-equivalence.json)；不把旧全量重新命名成新组合的全量。

## 尚未完成

正式性能点仍为 **0**。原单App大池密度场景没有达到逐worker预热覆盖；64并发、每worker冷1/热100、全后代PSS2048MiB和双次相等正值读取要求保留。单worker App的[私有替代方案](evidence/v0.3.0/2026-11-11-final-validation/density-private-scope.json)属于不同测量拓扑，实际8/65 worker比较排在本机Release队列后，尚未合入或认定等价。浏览器 host 不可用，1280/960/820/600四个实际视口仍未验收。

R3/R5/R6/R7/R8与最终R9继续按完整范围审查。下一步完成正在运行的Release及平台入口，审查正在运行的修正后Hosted CI，完成原密度预热方案、同会话正式三车道测量和四视口。已有通过项、原始失败项及各身份边界继续保留。

<details>
<summary>此前验收文档（按原字节保留）</summary>

# v0.3.0 当前验收与兼容边界

最近更新：2026-11-10T17:02:20.295835+00:00。当前源码的项目版本已升至 **0.3.0**，用于候选构建，尚未封板或发布。[证据索引](evidence/v0.3.0/2026-11-11-candidate-preparation/index.json)保存本次进展的不可变时点副本。最终候选的 Git、生成输入与二进制身份必须重新验收；下方旧结果继续保留各自身份。

## 当前验收边界与进展

正式 benchmark 和 2 GiB 密度验收继续使用本机 **CapsidV030 WSL**，不需要外部 SSH 主机。三车道原协议、四个 SUT CPU 和完整后代进程树 PSS 的 2048 MiB 预算保持不变。正式测量安排在构建及测试结束后的安静会话，记录实际 WSL 与工具身份；正式性能点仍为 **0**。

[namespace 原探针](evidence/v0.3.0/2026-11-11-candidate-preparation/namespace-native.json)在生产、ASan、TSan 三种真实构建中均通过。TSan 在专用 CTest 进程中直接执行完整探针，避免 sanitizer 后台线程与内部 fork 冲突；生产和 ASan 的 fork 路径及生产 sandbox 策略保持原样。

[实际内存场景](evidence/v0.3.0/2026-11-11-candidate-preparation/instrumented-resources-native.json)中，生产原 RSS 两项通过，ASan 的两个独立资源场景自然退出并完成 LSan，TSan 的两个独立资源场景自然退出且没有竞态报告。原始 RSS 断言及实际失败结果保留。用户已批准的 ASan 边界继续执行；TSan 采用相同的生产 RSS 与插桩资源分开检查方式，仍独立强制要求完整 ASan/LSan，不宣称 TSan 执行泄漏扫描。[报告修正](evidence/v0.3.0/2026-11-11-candidate-preparation/memory-report-claims.json)纠正了此前逐项元数据将 TSan 标为 `fullLsan=true` 的错误，两种真实结果均已用修正入口重新验收，原始日志未改写。

[CI 接线与本地控制](evidence/v0.3.0/2026-11-11-candidate-preparation/sanitizer-ci-controls.json)现在同时记录 ASan/TSan 的原始 RSS 退出码、完整 LastTest、JUnit，再强制执行其余矩阵和分别命名的资源场景。只允许精确的原始 footprint 断言失败按该边界验收；其他错误与 sanitizer 报告仍失败。Windows/Linux、LF/CRLF 共 **736** 项工作流控制通过，两端共 **16** 项 Python 测试与 **98** 项故障变异控制通过；这些本地控制不等于真实 Hosted CI。

[隔离 Python 工具](evidence/v0.3.0/2026-11-11-candidate-preparation/benchmark-python-tools.json)已恢复。[五种服务栈短时验证](evidence/v0.3.0/2026-11-11-candidate-preparation/benchmark-launchers.json)全部通过，验证了真实正文和 owned 进程清理。PHP-FPM/nginx 现在在 root 会话中明确沿用控制器身份以访问私有目录；非 root 路径保持原配置。轮数、请求负载和 PSS 稳定检查协议没有放宽。短时结果属于功能观察，不计正式性能点。

[隔离 Release/LTO 全量预备构建](evidence/v0.3.0/2026-11-11-candidate-preparation/preparatory-release-build.json)已通过，其源身份为预备快照。新构建暴露的动态 Boost.System 依赖已通过默认选择静态库修正；修正后的真实 ELF 与打包检查，以及最终候选全量，仍需重建后完成。

## 仍需完成

完成统一候选的干净构建、支持平台全量、包与复现、完整 sanitizer/fuzz/持续压力及真实 Hosted CI，再在本机安静 WSL 执行正式三车道性能、2 GiB 密度和四视口验收。Headers 与 MessageChannel 保留锁定 workerd 兼容边界，原始五项 WPT 失败和其中四项 TIMEOUT 保留；不能宣称全部 Web 标准测试通过。尚未推送、公开打标签或发布。

<details>
<summary>前次验收记录（按原文保留）</summary>


最近更新：2026-11-10T16:12:06.926566+00:00。项目版本仍为 **0.2.2**，v0.3.0 尚未封板。以下新进展以[本次证据索引](evidence/v0.3.0/2026-11-11/index.json)中的不可变副本为依据；较早完整快照保留在下方。代码与测试配置以私有快照 `3e55af775a0f2b07bcb53d2595e5a34abd6aed48` 为基准，本文与证据副本随后加入，最终组合身份尚待冻结。

## 当前决定与进展

正式 benchmark 和 2 GiB 密度验收使用[本机 CapsidV030 WSL](evidence/v0.3.0/2026-11-11/wsl-benchmark-decision.json)。不再等待外部 SSH 主机。测量安排在构建及测试任务结束后串行执行，记录实际 WSL 内核、CPU 绑定、工具版本、进程树与清理结果；这些结果将标明 WSL 环境。原三车道协议和完整后代进程树 PSS 的 2048 MiB 预算保持不变。

用户已确认[分开的内存验收口径](evidence/v0.3.0/2026-11-11/asan-memory-decision.json)：保留两项原始 ASan RSS 失败，由生产构建验收 RSS，由完整 ASan/LSan 及资源归还检查分别验收。[实际程序验证](evidence/v0.3.0/2026-11-11/asan-memory-native-profiles.json)中，生产 RSS 两项通过，原始 ASan RSS 两项失败保留，两个单独命名的资源场景通过；禁用泄漏检测、改变 quarantine、使用生产二进制等七项反例被拒绝。

[CI 接线](evidence/v0.3.0/2026-11-11/asan-memory-ci.json)已加入主工作区：先单独执行并保存原始 RSS 的退出码、完整 LastTest 和 JUnit，再严格执行其余 sanitizer 矩阵及两个资源场景。独立入口要求完整库存、自然退出、完整 LSan、默认 quarantine 与实际输入身份；只有原 RSS 断言的精确消息可以按该边界验收，其他场景错误和 sanitizer 报告仍失败。[真实原始结果的入口验收](evidence/v0.3.0/2026-11-11/asan-memory-actual-positive.json)通过；Windows/Linux、LF/CRLF 共 644 项工作流控制，以及两端共 12 项 Python 测试、86 项故障变异控制通过。这些本地控制不等于真实 Hosted CI。

[原始 TSan 全量](evidence/v0.3.0/2026-11-11/tsan-original-full.json)已结束：1087 项中 977 通过、109 失败、1 跳过，退出码 8，原始结果没有改写。四项 HTTPS 测试暴露了测试服务器中 listener context 释放早于接收线程退出的真实竞态；[关闭顺序修正](evidence/v0.3.0/2026-11-11/tls-fixture-race-fix.json)已合入，生产、ASan、TSan 共八项原始场景通过，包括四项 TSan 竞态场景，未加入抑制。生产运行时与原生依赖源码没有因该修正变化。

证书工具缺失默认配置阻止了部分 TLS 夹具进入产品场景。明确绑定系统工具后，[三种产品模式的原 TLS 边界场景](evidence/v0.3.0/2026-11-11/certificate-tool-replay.json)通过，[outbound/interop 四种角色共八项](evidence/v0.3.0/2026-11-11/tsan-post-full-focused.json)也通过。两项 benchmark 功能检查在全量结束后的单次原命令回放通过。TSan 的空闲 RSS 检查仍失败，namespace 探针仍返回 77，这两项尚未豁免。

本机 WSL 的[nginx、PHP-FPM 与 OPcache](evidence/v0.3.0/2026-11-11/native-benchmark-tools.json)已恢复，新增全局服务保持停止并关闭自动启动；[Deno 与隔离 Python 环境](evidence/v0.3.0/2026-11-11/deno-and-python-setup-at-capture.json)保留实际恢复状态及已校验的工具身份。正式性能点目前仍为 **0**。

## 仍需完成

继续处理剩余 TSan 验收差异，统一完整候选源码与生成身份，完成干净 Debug/Release/Linux LTO、支持平台全量、最终持续压力及真实 Hosted CI，再在本机安静 WSL 会话完成正式性能、2 GiB 密度和四视口验收。不得将旧身份的单项结果直接宣称为最终组合通过，见[需求账本时点副本](evidence/v0.3.0/2026-11-11/goal-requirements-at-capture.json)。尚未推送、公开打标签或发布。

<details>
<summary>上一份完整验收快照（2026-11-10 15:34 UTC，按原文保留）</summary>


本页记录 2026-11-10T15:34:00.106251+00:00 的验收快照。项目版本仍为 **0.2.2**，v0.3.0 尚未封板。主工作区未新增普通提交，未推送、打标签或发布。[证据索引](evidence/v0.3.0/2026-11-10/index.json)保存各份元数据副本的来源、采集时间和 SHA-256；正在执行的记录仅代表采集时点。

代码与测试配置以本地私有快照 `ef42e7bc5ae7c595ef6fb3ff39b64fc3f77f8f57` 为基准，共 5069 个常规文件；本文和证据副本随后加入。Linux 原全量使用 `56d4877`，Windows 原全量使用 `d251e93`，最终候选尚未统一冻结。主工作区的修改涉及测试校验器、Clang TSan 测试分支、测试链接配置和 CI；仓库内运行时、SDK、头文件、JS 及原生依赖补丁字节没有变化。新私有提交可能改变生成的 build identity，不能据此认定旧产物已经验证了最终身份。

## 已确定的行为

- Headers 保留锁定 workerd 的默认行为，并逐项记录与 Web 标准的差异，见[兼容与 sandbox 决定](evidence/v0.3.0/2026-11-10/headers-and-sandbox-decision.json)。
- MessageChannel 保留锁定 workerd 的 Promise 微任务投递；原始 WPT 的四项超时保留，另做严格兼容验收，见[调度决定](evidence/v0.3.0/2026-11-10/message-channel-decision.json)。
- Linux 生产 seccomp 策略保留。完整 ASan/UBSan/LSan 使用明确的 sanitizer profile，与生产策略分别验收。
- 两项 ASan RSS 验收边界仍待用户决定。资源归还和泄漏检查通过不能把原始 RSS 条件改写为通过。

## 实际执行结果

| 范围 | 已完成的验证 | 尚保留的限制 |
| --- | --- | --- |
| Linux 生产 | [完整 1097 项](evidence/v0.3.0/2026-11-10/linux-corrected-production-full.json)：1092 通过、5 项 WPT 文件失败、零跳过，原始退出码 8 | 普通全量仍为失败；五个标准冲突由独立兼容入口验收 |
| 当前参考与兼容 | [171 项必需参考门禁及完整 v2 审计](evidence/v0.3.0/2026-11-10/current-reference-and-wpt-profile.json)通过；[真实 1097 项结果的兼容入口](evidence/v0.3.0/2026-11-10/actual-complete-compatibility-acceptance.json)通过 | 保留五个文件、每模式 79 个子测试中的 23 项标准差异与四项超时；不声称完整 Web 标准符合性 |
| Linux ASan/UBSan/LSan | [1108 项原全量](evidence/v0.3.0/2026-11-10/linux-sanitizer-full.json)：1100 通过、8 失败、零跳过 | 五项 WPT 文件失败、两项 RSS 失败和一项已修正的端口校验误判均保留；不能称为原全量通过 |
| 原始 RSS 场景后续检查 | [两个实际场景](evidence/v0.3.0/2026-11-10/rss-terminal-diagnostics.json)完成其余原断言、回收及自然退出，默认 ASan quarantine 和完整泄漏检测保留，无 sanitizer 报告 | 诊断记录了原四个 RSS/VmSize 条件为失败，没有豁免。较早两次诊断因临时目录权限及 Unix socket 路径长度被拒绝，原记录保留 |
| 端口校验器 | [主路径 9246 项检查](evidence/v0.3.0/2026-11-10/native-oracle-main-integration.json)及[真实 sanitizer 26 组](evidence/v0.3.0/2026-11-10/native-oracle-fresh-sanitizer.json)通过 | 允许已关闭连接的端点元组复用，仍核对独立握手 nonce、请求/响应、清理和退出；原全量误判不改写 |
| Windows 原生 ASan | [842 项](evidence/v0.3.0/2026-11-10/windows-raw-full.json)：829 通过、5 项 WPT 失败、8 项跳过 | 后置整文件哈希校验拒绝该轮：打包重链接改变了[9 字节 PE 时间戳与 PDB age 元数据](evidence/v0.3.0/2026-11-10/windows-worker-pe-difference.json)。其他字节及代码段相同；[同环境打包预热](evidence/v0.3.0/2026-11-10/windows-package-prime.json)已稳定，最终冻结全量仍需执行 |
| 当前冻结源码 fuzz | [完整 LastTest 与编译图](evidence/v0.3.0/2026-11-10/current-full-fuzz-reuse.json)确认九项原测试通过、七目标各 10000 次、共 70000 输入，27 个编译单元启用 ASan/UBSan，21 个损坏完成记录被拒绝 | 复用已实际完成的运行；JUnit 成功输出被截断，计数来自完整 LastTest。当前 fuzzer 哈希采集于后续审计，不声称持有历史运行前的七份哈希 |
| 冻结状态模型 | [76 帧、30 条有序序列、168 步、39 项无效输入控制](evidence/v0.3.0/2026-11-10/bounded-state-model-controls.json)已核对 | 完整回放所登记的语料及双端竞态检查，不构成任意状态空间的穷尽证明 |
| TSan 依赖及产品构建 | [当前 OpenSSL r23 完整默认上游选集](evidence/v0.3.0/2026-11-10/tsan-openssl-r23-full.json)、[c-ares 全部 1151 项上游测试](evidence/v0.3.0/2026-11-10/tsan-cares-current-full.json)、[产品严格 ALL 构建及 13 项 focused 检查](evidence/v0.3.0/2026-11-10/tsan-whole-product-build.json)通过 | [原始 1087 项全量时点副本](evidence/v0.3.0/2026-11-10/tsan-whole-product-full-at-capture.json)及后续诊断保留各自状态，尚不构成最终候选全量验收 |

## 已合入的验收修正

[兼容 CI 及 sanitizer 分支](evidence/v0.3.0/2026-11-10/integrated-compatibility-and-sanitizer-branches.json)已经合入工作区。普通 CTest 原始退出码、失败名和失败步骤 outcome 保留；独立兼容入口要求完整库存、真实参考门禁和完整 v2 报告。其余 sandbox、打包和支持平台门禁仍须成功。本轮没有该候选的真实 Hosted CI 结果。

Clang 使用 `__has_feature(thread_sanitizer)`，此前没有选中仅检查 GCC 宏的既有 8 倍 TSan 测试驱动等待。[真实编译与执行](evidence/v0.3.0/2026-11-10/sanitizer-native-branch-controls.json)验证了修正：八组编译器分支控制、四个原 native 场景及四个 CTest 分类控制通过；TSan 下不可用的地址空间限制探针明确返回 77。Release、UBSan 和支持该探针的 Clang ASan 保留原 512 MiB 稀疏文件、256 MiB 地址空间上限及真实拒绝读取检查；GCC ASan 原先不执行探针却输出 PASS 的路径改为明确跳过。生产运行时期限及原 CTest 外层超时未改变。

[最新 CI 覆盖修正](evidence/v0.3.0/2026-11-10/integrated-ci-and-coverage.json)恢复三项此前被排除的 managed 检查；Windows/Linux、LF/CRLF 共 552 项工作流控制通过。证书夹具明确安装并绑定系统 `/usr/bin/openssl`，产品仍链接各自合格的插桩依赖。[四项真实证书工具对照](evidence/v0.3.0/2026-11-10/certificate-tool-binding-at-capture.json)确认专用 provider CLI 缺失默认配置会在进入产品场景前失败；显式配置和系统工具能生成并解析真实 SAN 证书，缺失配置反例被拒绝。产品三种模式的原 TLS 场景后续回放以该记录的实际状态为准。

## 封板仍需完成

还需完成原产品 TSan 全量与后续诊断、RSS 验收边界决定、统一最终候选输入和身份、干净 Debug/Release/Linux LTO 及支持平台全量、最终持续压力检查与真实 Hosted CI。Windows 原全量的冻结校验拒绝仍保留；任何单项通过都不关闭 R3/R5/R6/R7/R8 或 R9 整体要求，见[需求账本](evidence/v0.3.0/2026-11-10/goal-requirements-at-capture.json)。

正式性能发布点仍为 **0**。[命令准备](evidence/v0.3.0/2026-11-10/formal-performance-preparation.json)与[参数模板](evidence/v0.3.0/2026-11-10/formal-command-templates.json)已保存，安静的原生 Linux 测量环境尚待落实。WSL 功能验证不能充当正式性能结果。密度协议的 2048 MiB 指完整后代进程树 PSS 预算；每 worker 要求冷态一次、热态 100 次请求、两次相等正值读数和有界复核，达到配置上限只说明下界。正式同会话三车道五轮性能、真实密度与四视口验收仍开放。

较早 `54e274f` 的[持续压力记录](evidence/v0.3.0/2026-11-10/managed-websocket-1800s-soak.json)包含 1800 秒、4140 轮与自然退出，不能自动替代最终候选的验收。部分历史原始产物已按用户授权清理；[可用性审计](evidence/v0.3.0/2026-11-10/historical-evidence-availability.json)区分保留与删除的引用，复制元数据不表示重新核验了不可用的原始文件。

</details>

</details>

</details>

</details>

</details>

</details>
