# v0.3.0 当前验收与兼容边界

## 本轮推进（2026-11-13）：Admin HTTP 增量解析修复

**v0.3.0 尚未完成；正式性能点仍为0。** R1/R2/R4 保持定向完成，R3/R5/R6/R7 仍需组合验收，R8/R9 未完成。本轮工作区已增加下述修复，**不再等同于冻结候选23**。候选23的原本地全量队列仍在运行，本轮未向其发送信号、替换源码或改写产物；它不能覆盖本轮修改。

已修复 `src/host/admin_http.cc` 的两个独立问题：Beast `put()` 未消费的请求行/头/块边界字节被丢弃，以及非 eager 解析只处理首个 chunk 结构后丢弃剩余输入。原 `feed()` 的错误返回还会继续进入读取循环。现在保留有界输入余量，在头部 framing 校验后才开启 eager 正文解析，错误直接返回且不调用 backend；保留 peer 授权、原限额、绝对阶段期限和调用方 FD 所有权。未完成 chunk extension 的存储同样受限。真实读错误不再被误报为超时。

新增5项正式 Unix CTest，均保留10秒预算：分片请求头、合并/逐字节 chunked 正文、畸形 chunk 拒绝、分片限额、跨4096字节缓冲边界及未完成 metadata 上限。最终相同夹具下，旧实现 **16通过/5失败**；修复后普通组件和 ASan+UBSan 组件各 **21/21通过、零跳过**（13项 HTTP + 8项服务生命周期）。新增5项另在 ASan+UBSan 各重复10次，50次全部通过。结果为 Linux WSL/Clang18 的**组件验证**，依赖沿用候选23静态库，不能当作全依赖插桩或整版 ALL/全量通过。运行器编译错误、首版夹具的 unread-close reset 及重复验证发现的夹具竞态均保留；最终夹具只在发送结束后处理调用方关闭，不改解析器输入、状态断言或期限。详见[本轮输入、结果与限制](evidence/v0.3.0/2026-11-13-admin-http-incremental/review.json)。

公开页面已确认候选23 [CI 43417908994](https://github.com/ErosZy/capsid/actions/runs/43417908994)及[管理连接诊断43423254468](https://github.com/ErosZy/capsid/actions/runs/43423254468)均已结束且失败。本轮未逐项审校它们的新完整工件，因此不补造平台成功数，也不把本轮解析修复当作原 `EPIPE/ECONNRESET` 的根因证明。原 fuzz 60秒超时仍开放；未合入已被成本实验拒绝的分配提案，未增加任何预算。

接下来依次推进：

1. 审校候选23及管理连接诊断的完整原始工件，区分产品、夹具和环境失败；保留原失败，不用重试掩盖根因。
2. 定位并解决原预算 fuzz 超时；完成本轮管理传输修复的干净项目构建、macOS及所需检测配置验证。
3. 汇总所有改动后重新冻结候选，刷新受影响的 Host/managed 门及最终同源组合；继续核对下方 R1–R9，不能沿用候选23为新源码背书。
4. 完成资源/时序和1800秒持续运行后，再执行同一安静会话的 cold/QPS/density 正式三线与1280/960/820/600四视口验收。

本轮没有 commit、push、派发新CI、修改版本或发布。

## 上一冻结候选23记录（2026-11-13 12:02 UTC）

**最终组合尚未通过；正式性能点为0，版本未完成。** 当前冻结候选23为 `2e29006d3e85cd167a1c9224a12cded2cb2133dd`，源码树 `76cdb763c4819588765aeb73c4fd2bf708032e19`，共5163份源文件、SDK78。密度v2、独立原始证据重放及其故障控制已进入该候选；生产runtime与参考模型未改，原预算保留。完整CI [43417908994](https://github.com/ErosZy/capsid/actions/runs/43417908994)已启动，实际产品checkout为 `b38ed48c4dbde4b383e9ed7d72ea2cfd0b4c1901`，workflow为 `3d802afd4d98a2578c90c8b924b736c1a7ffb2e5`。实际产品树与本地冻结树精确相符，所有原平台、检测配置及预算与候选22工作流逐字比对，仅产品checkout身份不同。[冻结与派发证明](evidence/v0.3.0/2026-11-13-candidate23-freeze-and-dispatch/candidate.json)保留15份变更的输入摘要。

本地13份完整驱动已重绑候选23并核对哈希、语法、源码记录及原选择器；预计生产注册1115项（候选22的1114项加1项密度故障控制CTest）。新控制器PID `1333829`等待候选18 PID `482955`自然结束。仅退役已证明0步骤、无活动命令、无子进程、源码准备从未开始的候选22等待器PID `1211080`；活动测试和构建没有收到信号。首次交接预检因旧候选记录引用拒绝，发生于发送信号前，修正引用后重新完成全部守卫。[本地队列及原预检拒绝](evidence/v0.3.0/2026-11-13-candidate23-local-queue/replacement.json)可复核。

候选23的fuzz已独立审校为8/9通过，`fuzz-websocket-frames_smoke`在60.0297秒触发原60秒超时。失败种子4191328252，结束前至少观察到8618次输入；其余六个目标各完整10000次。27份编译单元均保留fatal fuzzer/ASan/UBSan，frame四份编译单元仍为原O2；原始二进制、前后语料和失败输出已归档。[原始超时审校](evidence/v0.3.0/2026-11-13-candidate23-fuzz-failure-review/review.json)不将未完成轮次计为通过。

[隔离的分配成本对照43419316434](https://github.com/ErosZy/capsid/actions/runs/43419316434)已经完成并独立审校：仅提案改变fuzz测试构造的容量预留、wire缓冲区写入方式及不可变反例fixture初始化；全部35行原invariant调用、整个fuzz入口和换行字节保留，解码器执行及无效输入取值不减，产品源文件未改。两份程序各三轮逐个重放同一份165项失败后语料；提案又完成原失败种子及两个固定种子的各10000次，并通过全部原9项、七个目标各10000次，完整fatal instrumentation和原预算保留。最长定向轮次44.6265秒，默认frame轮38.5851秒。相同种子没有被宣称产生相同的覆盖引导变异序列。[完整审校](evidence/v0.3.0/2026-11-13-frame-allocation-proposal/review.json)记录短语料重放约0.967秒、受子进程轮询精度影响，未证明成本下降，因此提案仍未合入。实际源文件保留CRLF；[聚焦变更视图](evidence/v0.3.0/2026-11-13-frame-allocation-proposal/source-allocation-only.diff)仅在显示时统一两侧换行，原较宽diff继续保留。

加长的CPU诊断[43420504648](https://github.com/ErosZy/capsid/actions/runs/43420504648)在原程序单进程重复大语料时失败：512 MiB RSS门保留，libFuzzer在1126次输入时报告604 MiB并以71退出，提案程序尚未执行；配置的3300次没有被计作完成。[失败审校](evidence/v0.3.0/2026-11-13-frame-exact-cpu-comparison/initial-single-process-failure.json)保留原始范围，不能外推为原随机smoke的内存结论。后续[43421076120](https://github.com/ErosZy/capsid/actions/runs/43421076120)以相同新进程重放165项语料、每组20进程，ABBA交错三组，已经完成并独立审校：每份程序实际执行19800次相同输入调用，原512 MiB、检测及quarantine设置和每组60秒保留。提案/原程序的CPU中位数比为1.009164，三组分别1.008084/1.009785/1.004468，未达到预先声明的5%成本下降条件，故拒绝合入。[成本审校](evidence/v0.3.0/2026-11-13-frame-exact-cpu-comparison/review.json)保留该负面结果；工作区fuzz源码仍为候选23原值。候选24只准备了带前置守卫的脚本，没有冻结、派发或排队，不能把该提案标为修复。候选23原超时仍未关闭。

候选22 TSan已独立审校为1007项中1001通过、5跳过、1失败：`websocket_ordinary_fetch_core_managed`第12个case（index 11，`ordinary-values-full`）在启动admin的GET `/v1/operations/op-admin-89906-0`时写入EPIPE。新增helper保存了请求、stderr和正常退出0的清理结果；失败发生在`startCapsid()`返回backend前，尚未证明是原ASan复位的同一根因。原目录资源场景十轮共20项通过，原始RSS两项中1通过、1失败另行保留；由于完整矩阵有失败，没有生成整体TSan资源接受。[完整失败审校](evidence/v0.3.0/2026-11-13-candidate22-tsan-failure-review/review.json)保留全部原始范围，没有按“未见TSan诊断”改判通过。

针对该失败的[隔离诊断43423254468](https://github.com/ErosZy/capsid/actions/runs/43423254468)已启动，绑定候选22原实际checkout。固定执行5轮原始core managed shard、10轮仅增加服务端连接结果和客户端socket状态的诊断版本、10轮系统调用跟踪版本；每轮仍为原30项、原120秒，无单次admin请求重试，所有失败逐轮保留。诊断改动仅存在私有工作流，当前工作区生产实现未改；不能用该定向系列代替最终候选的全量验收。

候选22 Linux Release独立审校为1114项：1105通过、5项原标准WPT失败、4项特权前提跳过；严格workerd门171/171，五份profile的20个实际模式见证已核对，版本化profile匹配，`ordinarySuitePassed=false`、`standardsConformanceClaimed=false`。五个原package测试均通过。包内二进制与build-tree严格门程序哈希不同，冻结安装器明确改写ELF RPATH；包的build-info与同源Release一致，但没有把两者声称为逐字节同一程序。该归档未包含严格门的未安装二进制字节，限制及两组摘要均记入[Release审校](evidence/v0.3.0/2026-11-13-candidate22-release-review/review.json)。特权步骤的完整日志审校仍待补齐。

候选22的三份完整归档已独立审校：[macOS](evidence/v0.3.0/2026-11-13-candidate22-macos-review/review.json)760项中749通过、11跳过；[ASan](evidence/v0.3.0/2026-11-13-candidate22-asan-review/review.json)1019项中1014通过、5跳过；[UBSan](evidence/v0.3.0/2026-11-13-candidate22-ubsan-review/review.json)1018项中1014通过、4跳过。注册与JUnit名称精确相符，原30秒参数控制保留19189个反例、10920组输入，三份归档分别14.3763/16.3379/21.2786秒；六项启动诊断控制均通过。ASan原始两项RSS失败及独立完整LSan资源接受分开保留，ASan/UBSan的provenanceDirty=1如实记录，不用于生产性能构建。macOS只注册single-worker/static-pool的native lifecycle，各100项；没有声称其覆盖Linux managed模式。旧候选的组件成功不替代候选23同源全量。候选21 ASan复验已审校为1013通过、5跳过，原启动复位根因仍未解释。完整R1–R9、本地资源/时序/1800秒持续运行、三条正式性能线及最终四视口仍未完成。

## 候选22回归记录（上一冻结，10:44 UTC起）

**最终组合尚未通过；正式性能点为0，版本未完成。** 当前冻结候选22为 `bc394d15980d7a3473ddde0c28b366e5ee5c7137`，源码树为 `763465bba4db12f906ce876255cad8d17fdc3951`，包含5159份源文件。新增组合 [43410461818](https://github.com/ErosZy/capsid/actions/runs/43410461818) 已实际启动Windows、macOS、Linux Release、ASan、UBSan、TSan和fuzz；workflow commit为 `365e4fd1689de923760195b9354e29fe55939b3b`，实际产品checkout为 `ce35c1b933a9984802b07b401b65f9cc8e73f995`，其树与本地候选22完全一致。fuzz已完成独立归档审校：9/9通过、七个目标各完成10000次，合计70000次；27个实际编译单元的fatal instrumentation和原单项预算保留，WebSocket frames为49.5233秒。其他六个执行job仍运行。[当前候选22 fuzz审校](evidence/v0.3.0/2026-11-13-candidate22-fuzz-review/review.json)保存二进制、运行前后corpus及实际构建身份；该fuzz构建的provenanceDirty=1也如实记录，不用于证明生产测量构建。

候选22相对21只变更参数控制的样本复制、启动失败诊断helper、六项诊断控制及其Unix CTest注册，生产runtime和参考模型字节保留。原30秒参数控制在两种Windows换行格式及Linux合计9轮通过后才冻结；旧候选21的全量失败仍保留。[冻结输入、实际diff与完整派发记录](evidence/v0.3.0/2026-11-13-candidate22-freeze-and-dispatch/candidate.json)和[本地完整队列交接](evidence/v0.3.0/2026-11-13-candidate22-local-queue/replacement.json)可复核。

以下表格属于上一轮冻结候选21 `c079fa1b4f616dba331c427269e39c2db79e8174`、树 `c6b13ee33695f9cc4970e198f11c921447a8ed80`，hosted run为 [43383568252](https://github.com/ErosZy/capsid/actions/runs/43383568252)。其workflow commit为 `cbbf9a87f0249c3414b76191d13b03fe54d54f68`，产品checkout为 `fab182f17cbce7bdc85e66e6f31ddfb2897362a3`。这些原始结果及已结束的旧ASan复验不能作为新候选22的完整接受。

| 上一轮候选21 hosted 验收 | 已核对结果 | 仍需处理 |
|---|---|---|
| fuzz | 9/9 成功 | 保留此前候选20的超时失败，不能用本轮覆盖历史 |
| UBSan | 1017 项：1013 通过、4 跳过、0 失败 | 4 个特权前提由本轮 Release 特权门交叉覆盖 |
| TSan | 1006 项：1001 通过、5 跳过、0 失败；注册名与 JUnit 相符 | 原 production RSS 失败保留；独立 TSan 资源接受不替代生产 RSS 或 ASan/LSan |
| macOS Release/LTO | 759 项：748 通过、11 平台跳过、0 失败；注册名与 JUnit 相符 | 跳过按原因保留 |
| Linux Release/LTO | 1113 项：1104 通过、5 项标准 WPT 失败、4 项特权前提跳过 | workerd profile 匹配，但 `ordinarySuitePassed=false`、`standardsConformanceClaimed=false` |
| Linux 特权及 delegated 环境 | 26/26 通过；4 个普通矩阵特权跳过均有对应通过；cgroup v2/network namespace 门通过 | 保存普通与特权结果的区别 |
| ASan | 首次 managed lifecycle 启动失败；同源码 attempt 2 为1018项，1013通过、5跳过、0失败，独立资源接受通过 | 原启动失败和两项原始 instrumented RSS 失败均保留；原 `ECONNRESET` 根因未解 |
| Windows | 冻结候选21为757项：748通过、1超时、8跳过；原生workerd参考门134/134通过 | 样本复制优化已在工作区完成，两种Windows换行格式及Linux共9轮原30秒定向门通过；完整新组合待验 |

本轮结果的逐项依据见 [TSan 审校](evidence/v0.3.0/2026-11-13-candidate21-tsan-review/review.json)、[macOS 审校](evidence/v0.3.0/2026-11-13-candidate21-macos-review/review.json)、[Release/WPT 审校](evidence/v0.3.0/2026-11-13-candidate21-release-review/review.json)、[ASan 失败审校](evidence/v0.3.0/2026-11-13-candidate21-asan-failure-review/review.json)和 [Windows 完整审校](evidence/v0.3.0/2026-11-13-candidate21-windows-review/review.json)。Windows 原始 job `131746692018` 已自然结束且失败，注册名与 JUnit 的757个唯一名称完全相符；归档中的三个真实程序摘要、MSVC Release 构建身份及134项参考门选择逐一核对。294份原始 driver report 保留，包括全部10份、每份26组的真实 Binding 参数报告；这些产品报告通过不能覆盖契约反例控制的超时。

ASan 首次失败的 `standard|prototype|default|release-eof` case 是第74项（index 73），在 `startCapsid()` 返回 backend 前抛出 `read ECONNRESET`，因此没有 `initial/headers/wire/observed`，上游尚无 TCP 记录。现有报告未记录具体 admin 请求、host stderr 或启动失败时的 shutdown 结果，根因仍未确定。未出现 ASan/LSan 诊断文本不能证明内存接受通过。GitHub 集成最初以 HTTP 403 `Resource not accessible by integration` 拒绝重跑；该回执保留，后续本机凭据通道已成功启动一次复验。

本机凭据通道最初也因整轮仍运行而被拒绝（HTTP 403，`The workflow run containing this job is already running`）。等待控制器确认整轮自然结束、原失败 job 与源码身份相符后，只发送一次开启 debug logging 的 POST，并取得 HTTP201及真实复验 job `131821551739`；attempt2已自然结束，1018项注册选择与JUnit名称精确相符，1013通过、5项因运行前提跳过。managed lifecycle全部100项通过并自然退出；原第74项也通过。独立ASan/LSan资源接受通过，原两项 instrumented RSS 失败仍保留，不能用于生产RSS结论。完整默认检测选项、8份真实核心编译记录和报告哈希已核对，没有加入抑制。该复验使用候选21旧helper，成功不能解释原复位根因，也不替代新候选。[独立复验审校](evidence/v0.3.0/2026-11-13-candidate21-asan-retry-review/review.json)、[原拒绝回执](evidence/v0.3.0/2026-11-13-candidate21-deferred-asan-retry/local-auth-request.json)及[成功启动记录](evidence/v0.3.0/2026-11-13-candidate21-deferred-asan-retry/deferred-observer-latest.json)分别保存；继承的其他平台任务没有被当作重跑。

启动失败采集已补齐并进入候选22：admin错误保存method/target/cause/code，响应中断会拒绝，启动清理后保存stderr、shutdown及cleanup failure。六个真实本地进程/Unix HTTP控制全部通过（部署复位、操作查询复位、非法JSON、截断响应、健康部署、非法readiness与非零退出）；请求期限、部署断言和重试行为保留。它们已进入新完整矩阵，但尚未证明原ASan失败的根因修复。详见[启动诊断修正及控制](evidence/v0.3.0/2026-11-13-managed-startup-diagnostics/review.json)。

Windows参数控制的同源独立诊断 [43407768456](https://github.com/ErosZy/capsid/actions/runs/43407768456) 已完成：未改测试完整执行28.094秒，带CPU profile执行31.235秒，导入约1.3秒。全部14个测试、19189个拒绝反例和10920组输入均保留；采样显示主要耗时在复制/反序列化样本。工作区提案改为深冻结、无对象别名的样本树，先验证JSON往返完全相等，再缓存其编码；每个反例仍生成独立可变图并重新调用原完整validator。所有原反例循环在统一换行后完全一致，新增控制拒绝值/类型丢失、未冻结分支与共享对象。

两平台各三轮原30秒CTest已通过：Windows22.210/22.062/22.030秒，Linux16.507/16.369/16.515秒；每轮均为14项、19189个原拒绝反例和10920组输入，0失败、0跳过。322份输入按平台核对，全部主参考捕获和validator字节均保留。Windows四份未锁定换行的源码在检出时仅由LF转换为CRLF；首轮提案测试文件使用LF，因此其“原循环字节相同”字段需限定为换行归一化后相同，原字段和原始文件都保留。

保留原Windows CRLF字节的[独立复验43408727395](https://github.com/ErosZy/capsid/actions/runs/43408727395)也已完成：22.217/21.954/22.019秒，三轮均通过原30秒门，原反例主体的物理字节与旧Windows文件完全一致。四份换行转换输入另保存实际字节，与冻结Git blob的精确LF→CRLF变换相符。合计9轮均保留全部反例和输入，没有增加时限或缓存报告接受结论。[原始CPU诊断](evidence/v0.3.0/2026-11-13-binding-arguments-windows-profile/review.json)和[九轮独立审校及实际diff](evidence/v0.3.0/2026-11-13-binding-arguments-lossless-copy/review.json)可复核。此修改已冻结进候选22，正在完整组合验收。

## 密度预热的当前实测提案

大池冷态预热的旧首批消耗完整 `8N` 请求预算，首批未覆盖全部 worker 时无法执行后续扩批。当前独立提案仅把首批缩为 `N`，沿用原无进展扩批逻辑；总请求上限、300秒覆盖期限、请求/响应字节、逐worker计数、PSS及清理断言均保留。新增故障注入控制证明原算法在首批只覆盖半池时失败，新算法在原预算内完成后续覆盖，另一个65-worker控制也通过。这些控制不构成真实密度接受。

真实10场景 [43403931697](https://github.com/ErosZy/capsid/actions/runs/43403931697)、job `131813434562` 已结束且失败：5/10通过。候选21 checkout `fab182f` 和已归档 Release/LTO 包（SHA256 `fd36d0b80bb0a0d3b3b8ab33da84c95da3b4d4a80fa7f9bbaa25d062a84f3828`）均有独立身份核对。23项密度控制及4项连接波次控制通过，但真实Capsid冷态65/128/256分别只触达43/95/77个worker，已校验响应数分别为520/1024/2048，均耗尽原冷态请求预算。热态65通过（逐worker至少103次、96915个正确响应）；热态128/256在后续波次读取超时。workerd65/128冷热四项通过。

已重解析全部计数序列、正确响应行和10个实际fixture，并核对所有Capsid READY事件先于首个业务计数、全部进程清理为0。热态128虽记录24344次业务执行，但只有16256次完成波次的已校验响应；热态256相应为23716和16128，失败波次不计预热成功。该提案不合入根源码，不能取得正式密度结果。

另发现继承的诊断模板将 `sourceTree` 写死为旧值；原字段保留并标为报告缺陷，GitHub commit API核对出的真实checkout树及包摘要单独记录。后续模板已改为从实际checkout读取树并做精确guard，未为已失败算法重派任务。[真实10场景审校](evidence/v0.3.0/2026-11-13-density-adaptive-budget/real-review.json)、[模板元数据修正](evidence/v0.3.0/2026-11-13-density-adaptive-budget/metadata-correction.json)和[原提案](evidence/v0.3.0/2026-11-13-density-adaptive-budget/proposal.json)保留全部结论。正式性能点仍为0。

后续仅用于诊断的 owned-worker barrier [43406627971](https://github.com/ErosZy/capsid/actions/runs/43406627971) 已完成并独立审计，10/10真实场景通过。方法是在预热期间只暂停具有精确PID/start身份和可执行文件路径的本次worker，Host保持运行；N个真实请求全部进入服务inflight后恢复worker，再检查原worker各执行一次。热态重复100波。65/128/256个Capsid worker冷热6项分别恰好达到逐worker1/100次，workerd65/128冷热4项也通过。303个Capsid波次均记录完整恢复信号，所有波次保持同一组worker身份；全部响应字节、计数序列、未改fixture、两次相等正PSS和退出后残留0均已重核。

该诊断保留生产调度、应用字节、请求预算、300秒覆盖期限、QPS64及PSS/清理判据，但改变预热执行时序，因此尚未合入或声称采用正式协议，也不计正式性能点。它证明可以完成逐worker预热；正式密度、全尺寸与全部参照仍需后续同会话验收。[10场景和303波次审校](evidence/v0.3.0/2026-11-13-density-owned-barrier/review.json)记录精确输入、全部原始摘要及适用边界。

保留原Python HTTP解析器的最小集成也已取得实际结果，避免把此前私有Go传输作为必需依赖。首轮私有任务[43409590756](https://github.com/ErosZy/capsid/actions/runs/43409590756)在控制测试阶段失败，尚未启动任何真实密度probe：遗漏了原计数观察fixture，同时首批请求数变更触发原520响应断言。原始失败保留；r2补齐原fixture，并还原整个原`density_probe.py`及全部22项控制的字节，仅在私有子类中实现预热屏障。

r2的[实际任务43409905726](https://github.com/ErosZy/capsid/actions/runs/43409905726)及独立审校均已完成：原22项控制、4项进程屏障控制和14项实际probe全部通过。Capsid65/128/256/512/768冷热10项各达到逐worker1/100次，workerd65/128冷热4项也满足原要求。505个Capsid波次均保持原worker身份并恢复执行，全部响应、两次相等正PSS和清理后0残留已重核。512个worker冷热PSS为1438566/1458944 KiB；768个为2152234/2181071 KiB，后者超过2048 MiB，是有效的超预算观察，不能算密度达标或正式性能点。[14项完整审校](evidence/v0.3.0/2026-11-13-density-python-barrier-integration/python-14-case-review.json)保留精确输入与边界。

此前提案的Capsid每波N请求，而workerd保留原先较大热态波次，没有据此发布两者的内存比较。随后[等价波次验证43411250200](https://github.com/ErosZy/capsid/actions/runs/43411250200)只将workerd首批收敛到N，使双方均能每波逐worker/cell执行一次；原Capsid算法和全部22项断言保留，本地控制通过。该验证覆盖65/128/512/768个workerd cell的冷热8项，已全部完成并独立审校：每波恰好N个请求，每个cell恰好1/100次，0响应错误、0清理残留，PSS均为两次相等正读数。warm768为2650673 KiB，超过2048 MiB，只作超预算观察。[等价波次完整审校](evidence/v0.3.0/2026-11-13-density-equivalent-conditioning-waves/review.json)保留输入绑定。密度v2随后已接入候选23，详见下节；冻结后的完整组合及三尺寸同会话正式对照仍需完成。[集成提案输入](evidence/v0.3.0/2026-11-13-density-python-barrier-integration/proposal.json)和[等价波次提案](evidence/v0.3.0/2026-11-13-density-equivalent-conditioning-waves/proposal.json)可复核。

## 本地串行队列

此前候选17至21按既有顺序排队；当前已将尚未启动的19/20/21收敛到新候选22，活动候选18保持运行。候选17未过滤TSan已自然完成：1105项中1098通过、6失败、1跳过，0超时、0Not Run；失败是原production RSS门及5个标准WPT文件。这属于候选17，不能作为新候选22的全量接受。

随后候选17生产模式 managed WebSocket soak 已通过：1800.212 秒、4274 轮、68384 echoes、2137 次部署和 worker 替换；最大 host RSS 25516 KiB，最终 channels/active/dials/queue/http 均为 0，退出 code 0、未强制终止。它是稳定性证据。详见 [候选17 TSan 与 soak 审校](evidence/v0.3.0/2026-11-13-candidate17-tsan-full-completion-review/review.json)。

候选18的 Release/LTO ALL 已以 exit 0 完成（strict-all 847 秒）。其未过滤、串行的1113项CTest已自然完成：1108通过、5项WPT失败、0跳过，原CTest退出码8；采集控制器的exit0表示完整保存结果，并不代表原测试全绿。注册名与JUnit的1113项唯一名称完全一致，48份二进制/provider/cache/定义输入及5158份项目manifest文件均重新核对摘要。五项失败是两个unhandled-promise、MessagePort初始禁用、Fetch headers basic/errors。详见[候选18完整Release审校](evidence/v0.3.0/2026-11-13-candidate18-production-full-review/review.json)。

候选18严格参考及版本化接受阶段也已自然完成：171/171通过，注册与JUnit名称精确相符，5158份源文件及51份守卫输入重新核对。取得的是保留五个原标准WPT失败的workerd profile接受，`ordinarySuitePassed=false`、`standardsConformanceClaimed=false`。控制器随后完成本地ASan/UBSan ALL并进入未过滤全量；原严格CTest PID `1117211`已退出。候选18结果不替代候选22。[候选18严格参考接受审校](evidence/v0.3.0/2026-11-13-candidate18-reference-review/review.json)保留输入绑定与原失败。

新候选22的13个完整驱动、精确tip/tree pack及源码manifest已经准备并核对。确认旧19/20/21的步骤均为0、没有活动命令、没有子进程、源码准备从未开始后，仅退役三个空闲等待控制器，原记录和输入逐份保留；没有向候选18或任何活动测试/构建发送信号。新候选22控制器PID `1211080`继续等待同一个活动候选18 PID `482955`自然结束，保留完整ALL、未过滤矩阵、171项严格参考、typed memory、时序与1800秒soak。新增一个Unix诊断CTest使生产注册预期从1113变为1114，源码数从5158变为5159；测试预算和原断言保留。[交接证明与驱动摘要](evidence/v0.3.0/2026-11-13-candidate22-local-queue/replacement.json)可复核。实时状态应结合控制器PID、活动子进程及对应`run.json`。

此前候选21队列预检曾发现两个未启动驱动仍读取候选20元数据；已派生只修正引用的source preparer/memory driver，并核对13个驱动。该候选21等待控制器PID `1032140`（r3）现已随上述空闲队列交接而退役，未启动源码或测试；修正后的驱动作为新候选22的派生基础，历史输入、产品快照/pack和全部原预算继续保留。[旧队列输入修正证据](evidence/v0.3.0/2026-11-13-candidate21-queue-input-review/review.json)保存替换前后摘要。

另已核对候选18至21的全部52份排队驱动：哈希与各自 preparation 清单一致、Python语法有效，source/memory 所引用候选记录均对应各自源码身份。[完整驱动预检清单](evidence/v0.3.0/2026-11-13-candidate21-queue-input-review/queued-driver-inventory.json)属于输入预检，不替代后续执行。该清单的09:39Z进程状态属于历史快照；Windows等待和旧候选队列随后已经完成或退役，以本页最新候选及独立复验审校为准。

## 已进入候选23的密度 v2

密度入口已接入逐实例、每波N个真实请求的预热：Capsid仅在派发屏障期间暂停已验证PID/start身份的自有worker子进程，Host继续运行；请求全部进入原生产调度后立即恢复，再读取响应、静置及采集整个进程树PSS。workerd按独立cell端口每波各发一次。生产选槽和runtime字节未改。暂停、派发、恢复都计入原300秒预热期限，每请求仍为5秒，QPS仍固定64并发；密度建连并发上限64，用户批准的扩大并发只作用于预热请求。恢复异常会继续尝试恢复其余全部自有worker并使本轮失败，后续清理保留实际信号记录。

独立证据校验器逐行重放原始响应、UUID/counter、READY日志、屏障、实际恢复信号和PSS集合，并拒绝重新封装的伪造证据。原22项密度控制保留，新12项故障控制、5项producer控制及28项独立证据控制均通过，共67项。[完整集成验证43414550837](https://github.com/ErosZy/capsid/actions/runs/43414550837)实际完成两栈、三尺寸、冷热共12个CLI场景（每项8实例），另完成Capsid 65/512/768冷热6个边界场景；所有实例达到精确1/100次，响应正确、清理零残留。768实例的PSS超预算，保留为观察。当前加严的READY状态校验器已对全部18个真实场景重新重放，并补验对应协议/状态反例。[独立审校及全部输入摘要](evidence/v0.3.0/2026-11-13-density-integrated-producer/review.json)保存原始scope与限制。

上述真实验证使用候选21的Release程序包及另行封装的工作区runner，属于集成预检。该改动已经冻结进入候选23，仍待同源全量验收，没有形成正式性能点或最大密度结论；三尺寸完整边界搜索、同一安静会话的三条性能线及最终四视口仍需完成。

## 完成门槛与下一步

完整范围仍以 [R1–R9 要求账本](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/requirements.json)及下方要求表为准：真实字节流与 BigInt 边界、native fetch 参数/owner/context/credit、Upgrade C ABI 限额、普通 fetch 与分支、DefaultReader、历史失败组合、各平台和依赖 instrumentation、资源/时序/分配失败/ABI、1800 秒持续运行，以及最终同源完整组合都需要对应证据。不能用旧候选或定向重跑替代最终候选的全量接受。

继续处理候选23的原预算fuzz超时并核对各平台完整归档，保留原ASan启动失败及其未解根因，让现有本地队列自然完成。随后对最终候选核对全部要求及构建/测试身份，再在同一安静会话执行 cold、QPS、density 三条正式线，保留候选、v0.2.2、pinned workerd 的原始输入输出。QPS 仍为 64 并发；cold 保留原尺寸、轮次及 warm 50；密度保留 2048 MiB、全部后代 PSS、两次相等正读数和退出证据。用户已批准密度预热按池规模扩大并发，逐 worker 验证冷至少 1 次、热至少 100 次；生产调度契约保留。

正式测量数据载入后，重新完成 1280/960/820/600 四视口验收。已核对本地 `capture-perf-site-four-viewports-r5.mjs` 和 `C:/Program Files/Google/Chrome/Application/chrome.exe` 均存在；该历史脚本仍标注使用历史数据、正式性能点为0，最终执行须绑定新数据和新的输出记录。浏览器连接工具当前不可用，本地 headless 路径可在资源阶段允许时执行。当前没有最终数据对应的四视口接受。完成验收不包含自动发布。

## 历史快照及逐次运行记录

更新时间：2026-11-13T05:51:35.412203+00:00。**v0.3.0 最终组合尚未通过，正式性能点为 0。** 当前冻结候选为 542ac467664c8aa9519b90c7250d88e5b6bba8d3，源码树 24328cc2d9c2a04c8d53d95077f4f34f2c306dfd，含 5158 份文件、SDK78；除独立更新的本文外，5157 份冻结输入与工作区逐字节一致。工作区 HEAD 仍为 8028b88adff26cbc1abcb9a2fafce2ffc3d8090f。

本轮已合入目录描述符修正：四个有界目录验证 helper 使用已有的状态描述符同步域，验证失败路径直接在已持锁范围内关闭描述符，避免重复加锁。worker 启动、激活和请求执行不在新增锁范围内；生产选槽规则与请求处理代码保持原值。原部署 close / 健康检查 fstat 的 TSan 失败完整保留。相同修正源码的私有 ASan 1018 项中 1013 通过、5 项声明跳过；UBSan 1017 项中 1013 通过、4 项声明跳过，JUnit 名称集合与实际注册一致。两项原始 ASan RSS 失败仍保留，独立资源接受核对了输入哈希、自然退出和完整 LSan。十轮、每轮两项原 TSan 资源场景的 CI 步骤已返回成功；原同源 TSan 完整矩阵及逐轮原始归档仍待结束、审校。这些是有界修正的定向依据，最终完整接受仍未成立。

当前候选完整 CI [43374106019](https://github.com/ErosZy/capsid/actions/runs/43374106019) 已派发，实际干净检出 320a7966b733f12c7633bbfadb82c910471b1b55，产品源码树与冻结候选一致。当前实际 fuzz 已通过 **9/9**，七个目标共完成 **70000 次**，WebSocket 项 39.737 秒；归档、27 份实际编译单元的检测选项及原单项预算已核对。所有原平台、三种检测配置、选择器与预算保留，并在本候选的 TSan 中再次执行十轮原资源场景，每轮保留 JUnit、LastTest 与资源报告。三个 Linux 安装步骤改用已实测可用的 Ubuntu 官方签名软件源，下载源主机名以外的依赖、签名检查、构建与测试命令均保留。此前镜像连接失败及 GitHub 在整组运行时拒绝原地重跑的记录保留。

本地当前候选控制器 PID 650215 为 waiting-for-live-parent18-queue-natural-completion，等待候选 18 的冻结队列自然结束；候选 18 仍等待候选 17。候选 17 当前为 strict-reference-and-acceptance-running，其 Release 1113 项未过滤执行已经结束，正在严格 workerd 对照阶段。当前候选的 13 份冻结驱动已准备并入队，完整构建、未过滤矩阵、严格对照、资源、时序及 1800 秒持续运行继续严格串行。前序运行和排队输入均保留。

此前候选 075fee1 的 fuzz 已独立审校为 9/9，WebSocket 项 35.31 秒；macOS 759 项中 748 通过、11 项声明跳过；UBSan 1017 项中 1013 通过、4 项声明跳过。fuzz 的 -O2 调整、全部原 harness 检查、七个目标各 10000 次及原 60 秒门槛继续保留。该候选的其余 Linux、Windows 与 TSan 结果继续独立收集，不能替代本轮新候选。[此前构建优化及原始资源失败](evidence/v0.3.0/2026-11-13-frame-optimized-and-directory-review/checkpoint.json)保留原身份。

密度的两项新私有提案均为 6/10。分段发送完整原请求头，65 实例 Capsid 冷热态及 workerd 65/128 冷热态通过；Capsid 128/256 冷态分别只触达 72/71 个 worker，热态读取超时。再将热预热首批限制在每 worker 8 个请求、保留原无进展扩批后，128/256 冷态分别触达 95/63 个 worker；128 热态日志已观察到全部 128 个业务 UUID，但后续批次仍触发原 5 秒读取期限，未取得热态通过。日志中的业务调用数没有充当成功响应数。两组各十个场景已逐行重放连续计数，逐字节比对生成 fixture，确认所有 Capsid READY 事件先于首条业务计数，所有进程清理为零残留。原请求总量、300 秒覆盖、冷 ≥1 / 热 ≥100、精确响应及 PSS 校验继续执行；这些提案尚未合入，也没有作为最大密度或正式 PSS 接受。

四视口的最终验收仍需正式测量数据。已找到此前成功使用的本地独立 headless 截图脚本与 Chrome 文件，可在数据就绪、资源阶段允许时复用；本轮未重新启动该浏览器，不声称取得新的视觉通过。此前浏览器连接工具不可用的原始事实继续保留，但不再将连接用户浏览器列为唯一执行路径。[本轮源码、资源修正、平台和密度检查点](evidence/v0.3.0/2026-11-13-managed-directory-and-density-correctness/checkpoint.json)保存实际身份和结果。完整 R1–R9、最终同源组合、三条正式性能线、最大密度及最终四视口仍未完成，未发布。以下历史分节保留各自候选身份。

## 全部要求及当前状态

完整要求、状态和历史引用保存在[原始要求账本](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/requirements.json)和[更新前完整账本](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/goal-ledger-before.json)。下表保留 R1–R9；定向完成项仍须与最终组合对应。

| 要求 | 范围 | 状态 |
|---|---|---|
| R1 | 标准字节流部分/未对齐 EOF、零 EOF backing、高 minimum 取消及 36 个延迟组合 | 定向要求完成 |
| R2 | BigInt limb 边界、转换顺序、分配限制、独立回归及上游来源 | 定向要求完成 |
| R3 | 真实 native fetch body 参数、released/detached/invalid view、全部 view/构造路径、重试/数据/owner/credit/退出、并发/context/abort/intrinsics | 组合验收进行中 |
| R4 | 真实非零 C ABI response-body 限额经 Upgrade，known/unknown 长度及 below/equal/above、Default/BYOB、取消与回收 | 定向要求完成 |
| R5 | 普通 fetch 元数据及 tee/clone/values/pipe 分支、资源与 reader 取消归属图 | 组合验收进行中 |
| R6 | DefaultReader 精确 chunk 序列/时序、额度控制、Linux rich-JSON invalid IPC frame 根因 | 组合验收进行中 |
| R7 | 历史 15 个失败组合、真实跳过前提、原始失败与 WPT/profile 证据 | 组合验收进行中 |
| R8 | 干净 Debug/Release/Linux LTO/CI、依赖 instrumentation、fuzz、持续运行、正式性能/密度及四视口 | 尚未完成 |
| R9 | 完成 R1–R8、冻结完整候选并执行最终组合接受；发布仍需授权 | 尚未完成 |

## 本轮已验证修正

冷启动工具在公开 C ABI 回收直接 worker 后，再回收自身收养的后代，成功样本须在清理完成后输出。真实 fork/clone 包装器随后 exec 原生 worker，覆盖 SIGINT、SIGTERM、正常输出、错误响应和超时。原断言继续在外围夹具清理之前检查整树。GCC ASan、Clang ASan/UBSan、Release 各通过原 7 项及全部 10 个后代场景，旧实现各被 10 个断言拒绝；新增两项正常场景各自保留 30 秒测试预算。

Clang 空闲 PSS 的映射对照中，默认一秒静置期间总增量 764 KiB，其中 680 KiB 落在 FakeStack 映射。同一 worker 保留默认 instrumentation、静置 20 秒后取得两次相等正 PSS。因此只为 Clang ASan 功能测试配置 20 秒采样前静置；原 100 秒 CTest、两次完全相等、最多 8 次采样、采样间隔及取消期限保留。Clang 原 7 项实际 65.7 秒，两项正常后代各 21.4 秒。关闭 FakeStack 的诊断对照不计验收，生产 cold 默认参数和正式测量协议保持原值。

三种构建的 4 个 CTest 均通过。临时 CI 的后置收集器因 unittest 进度点与 JSON 行交错而失败；原失败记录保留，独立复核恢复全部 10 行及正负控制，没有重写原结果。[三个构建审计](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/cold-three-profile-review.json)和[原始内存映射对照](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/fake-stack-mapping-review.json)可复核。

真实 Ubuntu runner 上安装并选择实际 perf，四个软件计数器探测通过；构建真实 Host/worker 后，原 A/B 证据控制在原 600 秒预算内输出 PASS。CI 复用该已执行的 helper，完整 sanitizer 矩阵另保存原始场景 sidecar，测试选择和单项时限不变。这不是产品性能点。[实际 perf 前提](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-prerequisite.json)、[控制输出](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-controls.stdout.log)和[真实程序摘要](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/actual-perf-identity-fixtures.sha256)保留。

## 完整结果与未通过项

下列完整结果属于父源码 `63a3cc1`，不替代本轮新组合：Linux Release/LTO 完整 ALL 通过；未过滤 1110 项为 1105 通过、0 跳过、5 项原 WPT 差异。同源严格 workerd 171 项全部通过并取得版本化兼容接受，原 5 项差异与 4 项超时保留，ordinarySuitePassed 和 standardsConformanceClaimed 仍为 false。[完整接受记录](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-linux-compatibility-acceptance.json)保留输入绑定。

父源码 UBSan 为 1009 通过、0 失败、5 个原声明跳过；9 项 fuzz、7×10000=70000 次及 27 个相关编译单元已审计。Alpine 3.24 原 Docker 发布配方的 9 项原生/包消费测试通过，3 个归档程序独立解析 ELF 后均无解释器或共享库依赖；这不等于完整 musl 全量或参考接受。完整原始归档及 SHA256 仍在此前日期目录和工作区保存，[此前完整审校历史](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/qualification-before.md)逐字节归档。

父源码 ASan 1015 项中 1008 通过、1 失败、6 跳过，失败为取消冷启动后的僵尸后代；本轮已独立修正，尚需新组合完整验证。TSan 1003 项中 994 通过、3 失败、6 跳过，失败为 native_fetch_large_gzip 的辅助状态观测及 managed 的 native-reader-lifecycle、reader-scope 完整采集/清理；[TSan 原始审计](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-tsan-review.json)未改写。保留原源码、依赖构建、测试与时限的补采任务已完成，3 轮各 3 项均通过并保存完整 sidecar；这些定向通过未当作原全量失败的修复。

Windows 原 workerd 134 项为 133 通过、1 失败、0 跳过，失败发生在 pinned workerd 的 codec-supp-fragment-crc-late 源 EOF 观测。同输入、同二进制的 3 次复查各 31 场景通过，未当作修复；[逐次记录](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/windows-same-input-replays.json)保留。macOS 父候选 fa40606 曾完成 758 项、747 通过、11 个原声明跳过；63a3cc1 首次构建通过后，临时采集器误调用 Ninja 导致测试未启动；修正采集方式、保留原 Unix Makefiles 生成器和测试预算后，已完成 759 项、748 通过、0 失败、11 个原声明跳过，并保存 275 份原始 driver report。当前 14fbabb 的独立 macOS 矩阵也已完成并审计：759 项、748 通过、0 失败、11 个原声明跳过，275 份原始 driver report 保留。

## 剩余执行与正式协议

父源码本地 Clang ASan/UBSan 全量已自然结束并保留全部失败；未启动的旧 TSan/资源计划已交接到当前候选，当前干净 Linux Release/LTO ALL 已完成，完整原始测试与后续资源验收继续串行执行。必须先解决上述 TSan 和 Windows 原参考失败，并完成当前源码原生平台、依赖 instrumentation、资源回收、时序、ABI/旧接口、分配故障及 1800 秒持续运行，再取得最终组合接受。

正式测量仍需同一安静会话的候选、v0.2.2 和 pinned workerd 对照及全部原始输入/输出。QPS 保持 64 并发；cold 保留原尺寸、轮次和 warm 50；密度保留 2048 MiB、全部后代 PSS、两次相等正读数与退出证据。已批准按池规模扩大密度预热并发，逐 worker 验证冷至少 1 次、热至少 100 次后采样，生产调度不变。[父候选三条正式命令输入核对](evidence/v0.3.0/2026-11-12-cold-settle-and-perf/parent-formal-input-plan.json)属于准备，正式性能点仍为 0。

1280/960/820/600 四视口已有历史真实截图，正式数据载入后须再次验收。全部 R1–R9 完成并与同一个最终候选绑定后才能宣布版本完成；不自动发布。

父候选 `14fbabb` 的[完整 CI](https://github.com/ErosZy/capsid/actions/runs/43232371737)已结束且未通过，精确 checkout `704d1c864202dda18f1d9dc00e22c9a182686ea2` 的树与该父候选相同；所有原始产品任务、单项预算与最终汇总规则保留。[本轮源码与验收检查点](evidence/v0.3.0/2026-11-12-cold-settle-integration/checkpoint.json)区分组件通过、父候选结果和当前待验项。

当前 `14fbabb` 的 9 项 fuzz 已完成独立审计：7 个随机目标各真实完成 10000 次，合计 70000 次；27 个编译单元、原 60 秒 CTest、单输入 2 秒、64 KiB 输入及 512 MiB RSS 上限均保留。该流水线其他任务最终未全部通过。[当前 fuzz 审计](evidence/v0.3.0/2026-11-12-cold-settle-integration/current-fuzz.json)与[原始归档](evidence/v0.3.0/2026-11-12-cold-settle-integration/current-fuzz-original.zip)可复核。

本地父源码 `63a3cc1` 的另一组完整 Clang ASan/UBSan 结果为 1114 项执行、1052 通过、62 失败、0 跳过，原始结果没有转换为通过。其中 56 项属于 managed 场景；旧临时目录下典型 admin socket 路径为 114 字节，超过 Linux 108 字节容量。同一原始 `host_ws_upgrade_gates_managed` 命令、同一二进制与原 45 秒预算在长目录失败、短目录通过，完整源文件、依赖、CTest 定义和 instrumentation 选项未变。该因果对照只证明这个启动前提，56 项仍须由当前候选完整重验；原 5 项 WPT 差异和 1 项旧 cold PSS 失败保留。[独立复核及原始结果摘要](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-local-asan-review.json)、[完整原始归档](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-local-asan-original.zip)和[长短目录因果对照](evidence/v0.3.0/2026-11-12-current-local-handoff/same-test-long-short-controls.json)可复核。

父候选 `14fbabb` 已在全新 Linux checkout 验证 5155 份源文件，并完成生产 Release/LTO ALL、12 个 LTO runtime 编译单元与 1113 项真实注册。只有等待中的控制器收到信号，原运行测试和本轮 ALL 均自然结束；后续未启动脚本使用独立短临时目录，原断言、测试选择、instrumentation 和时限保留。新的三个 20 秒 Windows QPC/WSL 时钟窗口全部通过。当前完整测试、严格 171 项对照、完整 ASan/UBSan/TSan、资源、时序和 1800 秒持续运行仍需全部通过；正式测量点仍为 0。[当前本地验收检查点](evidence/v0.3.0/2026-11-12-current-local-handoff/checkpoint.json)记录同源构建身份与交接范围。

当前 `14fbabb` 的正式输入准备已实际完成：15 份 cold fixture 摘要、11 个真实二进制身份、5 组官方 tag/ref 及三条真实 CLI 参数解析全部核对。候选 Host/worker 摘要绑定本轮 Release/LTO ALL；尚未建立安静会话，未创建会话 metadata，也未运行正式测量。[当前输入与命令检查](evidence/v0.3.0/2026-11-12-current-local-handoff/current-formal-cli-plan-check.json)属于准备证据，正式性能点仍为 0。

父候选 macOS 补验的 41,075,216 字节原始归档已按 GitHub artifact 摘要核对，JUnit 全部 759 个名称与实际注册逐一相符，11 个跳过项保留各自原始声明。编译数据库和 Makefiles 链接命令均已采集；首次采集器失败记录仍保留。[macOS 完整复核](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-macos-complete-review.json)与[原始归档](evidence/v0.3.0/2026-11-12-current-local-handoff/parent-macos-original.zip)属于父候选证据。

当前 `14fbabb` 的本地 Release/LTO 未过滤全量已实际结束：1113 项执行、1108 通过、5 项原 WPT 差异、0 跳过，严格 workerd 组合接受随后已完成。该候选 macOS 的 759 项也已独立审计，748 通过、0 失败、11 个原声明跳过；本轮记录与父候选补验区分保存。

同源 hosted Release 的结果为 1113 项执行、1099 通过、10 失败、4 个原 sandbox 声明跳过。原 A/B 证据控制和真实 perf 前提已通过，严格 171 项也全部通过；另外 5 个失败是两项 retired ownership 参考采集与三个 PSS/控制器测试，因此完整版本化接受仍被正确拒绝。ASan 的 1018 项中有 5 项失败，UBSan 的 1017 项中有 4 项失败，涉及控制器/PSS、原 30 秒参数契约控制及 managed reflect-standard 完整采集。报告目录中的 POSIX 冒号又导致这两个归档上传失败；完整原始 job log 已保留，已启动只修正归档方式的三种完整 sanitizer 重采，代码仍为父候选 `14fbabb`。[本轮完整结果及边界](evidence/v0.3.0/2026-11-12-current-matrix-findings/checkpoint.json)保留这些失败，不以组件通过替代组合结果。

Windows 当前完整参考门仍为 134 项、133 通过、1 失败、0 跳过，重复发生在 pinned workerd 的 codec-supp-fragment-crc-late 实际源 EOF 观察；97 字节解码与 Gzip 错误状态均被原始报告保留，尚未证明修复。该参考关闭问题需继续实测诊断。

历史检查点（合入前）：私有提案 `1fb5e29` 保留全部原 native 公共状态/真实上游断言、benchmark 程序与原测试时限，仅修正参考监听端口、8 项 CTest 调度属性及报告打包方式，尚未合入工作区。真实 pinned workerd 的占位端口对照中，旧 TCP 就绪检查接受外部服务并收到 201，新子进程控制通道选择实际监听端口并取得 200；原 48 项和新增 7 项回归控制全部通过。带冒号路径、符号链接和一份真实保留报告的归档读回也通过。四种构建的 12 项原测试各 3 轮观察已启动，结果仍待审计；内部双控制器并发场景保持不变。[实际端口对照](evidence/v0.3.0/2026-11-12-current-matrix-findings/actual-pinned-workerd-port-control.json)和[私有修改](evidence/v0.3.0/2026-11-12-current-matrix-findings/private-port-and-isolation-candidate.diff)可复核。正式性能点仍为 0。

`1fb5e29` 的四种构建定向结果现已全部独立审计并合入工作区：Release/LTO、GCC ASan、GCC UBSan、Clang TSan 各 3 轮、每轮 12 项原 CTest，共 144 项通过、0 失败、0 跳过；同时保留 1272 个 managed BYOB 和 144 个 native retired ownership 原场景。实际注册确认 8 项 RUN_SERIAL 属性，原时限、真实 PSS 判据、内部双控制器并发和所有原语义断言保留。工作区的 55 项 native/监听控制和 workflow 契约也通过。[同源合入记录](evidence/v0.3.0/2026-11-12-port-and-pss-integration/integration.json)区分定向资格与尚未完成的整版验收。

父候选 `14fbabb` 的本地 1113 项未过滤全量及严格 171 项已完成版本化组合接受，原 5 项 WPT 差异与 4 项历史超时仍保留；该结果不替代新源码的完整组合。当前同源 hosted TSan 的 1006 项中仍有 1 项 native_fetch_large_gzip 状态采集失败，报告上传也被 POSIX 冒号拒绝；原日志与 64 KiB 截断原行保留，完整原侧录待三种 sanitizer 重采返回。Windows 的 4 次隔离观察均通过，但原完整门两次 CRC-late 源 EOF 失败仍未解释，状态查询时序调整仅用于诊断，未合入。完整新候选平台、instrumentation、资源、时序、1800 秒持续运行、正式性能三条 lane 与四视口仍需验收，正式性能点为 0。


最新补采已独立核对完整 ZIP 与内部 tar 摘要：父候选 `14fbabb` 的 ASan 1018 项为 1012 通过、1 项原 30 秒参数契约控制超时、5 项有原始原因的声明跳过；另行保留两项原 RSS 失败，未声称资源接受。UBSan 1017 项为 1010 通过、3 项 PSS/控制器失败、4 项声明跳过。两种构建的 managed reflect-standard 各 106 个原场景与两组大 gzip 场景均通过。真实含冒号的运行目录及符号链接已完整进入归档，报告收集修正已由完整任务验证。这些是旧源码补采结果，不替代 `1fb5e29` 的整版验收。[最新检查点](evidence/v0.3.0/2026-11-13-progress-status/checkpoint.json)、[ASan 完整复核](evidence/v0.3.0/2026-11-13-progress-status/recovered-asan-review.json)和[UBSan 完整复核](evidence/v0.3.0/2026-11-13-progress-status/recovered-ubsan-review.json)保留全部原失败。

Windows 隔离诊断的 4 次 31 场景均通过，其中原观察顺序也两次通过，因此未证明改动查询顺序是修复。新的完整 134 项诊断以新候选的实际代码构建，仅在失败清理之前补采上游状态；原断言、3 秒 EOF 观察及全部预算保持，诊断不计最终资格。本地父候选 Clang ASan/UBSan 的完整 1117 项仍在串行执行，云端完整 TSan 补采仍待结束；最新状态记录保存实际 PID 和 run ID。最终仍需新候选全部平台、资源与时序、1800 秒持续运行、同会话正式 cold/QPS/密度及四视口，正式性能点为 0。


R6 的历史 IPC 根因审计备份已重新定位，文件 SHA256 与原账本的 `39ea538b…` 完全相同，原审计程序摘要也匹配。该记录包含原始 Host/worker 摘要及原始、条件修正、恢复三组对照：已经通过 registry 验证且标为 discard 的 DATA 在旧 handler 落入拒绝分支，导致 503/invalid IPC；仅修正条件跳转后，原 14 场景通过，恢复后再次失败。当前源码仍保留已验证 DATA 的允许路径和无效帧拒绝检查。原审计记录的副本现可读取，但此前获准清理的原始追踪及二进制未在本轮重新验证；这不是新的运行证明，也不关闭 R6 的精确 chunk/时序与最终组合要求。[历史审计副本](evidence/v0.3.0/2026-11-13-historical-ipc-reference/original-audit-snapshot.json)与[本轮可用范围复核](evidence/v0.3.0/2026-11-13-historical-ipc-reference/review.json)明确区分两者。


父候选 `14fbabb` 的完整 TSan 补采现已自然结束并独立审计：1006 项实际选择名称与注册逐项相符，1001 通过、0 失败、5 项原声明跳过。8 MiB 与 96 MiB gzip 的完整原始 JSON 均通过未改动的严格契约，分别解码 8388608 与 100663296 字节，源端 EOF 在清理前观察到，worker 正常退出；managed reflect-standard 的 106 个场景也通过完整契约。独立 TSan 资源接受的输入摘要、同一实际 Host/worker、运行时标记和正常退出均复核通过；原 reclamation-idle RSS 失败继续保留，生产 RSS 和完整 ASan/LSan 仍各自要求。此前完整任务的 missing-state 失败未被改写，也未宣称已找到原因；这次完整通过属于父候选，最新 `1fb5e29` 的最终整版验证仍待执行。[完整补采检查点](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/checkpoint.json)、[注册及资源复核](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/complete-matrix-review.json)和[完整语义契约复核](evidence/v0.3.0/2026-11-13-complete-tsan-recovery/full-contract-review.json)保留准确范围。三种 sanitizer 的补采任务均已结束，ASan 的原 30 秒控制超时和 UBSan 的三项 PSS/控制器失败仍保留；Windows 完整诊断与本地完整 Clang ASan/UBSan 仍在执行。


首次当前 `1fb5e29` 独立密度预热观察的派发检查点如下，后续结果及观测器修正见末段。实际 checkout 的树与冻结候选一致：将分别验证 capsid 的 65 个 worker 和 workerd 的 65 个 cell，冷至少 1 次、热至少 100 次后，再取得整树两次相等正 PSS 和零残留 PID。保留原 300 秒覆盖期限、冷最多 520 次和热最多 208000 次请求；65 个实例的预热并发上限为已批准的 4160，QPS 仍为 64。此前 65 实例冷/热覆盖与 PSS 通过记录属于 `2979e33` 二进制和较早脚本，当前脚本摘要已不同，故未当作当前候选结果。新任务属于工具与预热资格观察，尚无结果，也不计正式性能点或最大密度。[当前源码任务及范围](evidence/v0.3.0/2026-11-13-current-density65-dispatch/checkpoint.json)与[实际派发记录](evidence/v0.3.0/2026-11-13-current-density65-dispatch/current-density65-dispatch.json)保留输入绑定。

Windows 原完整 134 项诊断仍在执行。独立真实 TCP 子进程对照确认，FIN 会触发 Node `end`，复位会触发 `ECONNRESET` 和 `close` 而没有 `end`；该对照未改变夹具主动结束或销毁状态。锁定 workerd 源码中的解压错误与 reader 错误传播路径也已读取，但二者都不足以证明本次 workerd 失败的具体传输终止方式。必须继续检查原完整任务的清理前上游侧录，codec 断言和全部原时限尚未修改。


首次当前源码密度任务 `43260447861` 已自然结束，完整日志与归档均保留。冷阶段实际 65 个 worker 均收到请求，520 个响应全部正确，逐 worker 最少 1 次，整树 PSS 为两次相等的 197993 KiB，清理后零残留 PID；原始计数日志已独立重解析并与结果逐项核对。随后私有观测脚本错误地要求启动参数存在 `--strict`，而实际工厂使用 `--mode managed`，因此任务失败、其余三个阶段未执行；没有将其声明为四阶段通过，也没有中断原测试。修正仅涉及 CI 观测断言，真实归档命令通过，错误模式和参数两项反例均被拒绝。修正版 `43262279072` 已以同一冻结源码树重新派发四个阶段，原请求上限、300 秒期限、PSS 与退出要求保留，结果仍待收集，正式性能点仍为 0。[原始失败及冷阶段复核](evidence/v0.3.0/2026-11-13-density65-observer-correction/original-observer-review.json)、[修正版预检](evidence/v0.3.0/2026-11-13-density65-observer-correction/corrected-observer-preflight.json)和[当前检查点](evidence/v0.3.0/2026-11-13-density65-observer-correction/checkpoint.json)明确区分这些范围。


私有提案 `bc61dbe` 只修正 CRC 完整错误响应后的已观察复位分支、补采失败侧录及预热并发上限。CRC 之外仍要求原 FIN；源端全部分片、字节、端口、公开状态、清理前终止和原 3 秒观测器均保留。新增控制拒绝缺片、夹具主动销毁、错误端口、无复位及其他 codec 场景。密度提案仅将临时并发改为 `max(64, 128 * pool_count)`，不改 fixture、生产调度、QPS64、请求总上限或 300 秒期限；另保留真实 208000 响应却不足 100 次/worker 的反例。该较大波次仍待实际验证。[提案差异](evidence/v0.3.0/2026-11-13-reset-density-and-local-asan-findings/private-proposal.diff)和[全部本地控制](evidence/v0.3.0/2026-11-13-reset-density-and-local-asan-findings/private-proposal.json)可复核。

同一私有树的任务 `43270591475` 已派发：Windows 用原完整任务中摘要核对的真实生产二进制执行两轮各三种模式的 31 个 metadata 场景，共 186 项，原单项 120 秒预算不变；这属于新验收器与已保存二进制的组件验证，不是新源码完整 ALL。Linux 重新构建该候选，执行 65 个 worker/cell 的四个冷热阶段。工作区仍为 `1fb5e29`；旧源码完整 TSan 的 1105 项继续运行，后续资源、时序、1800 秒持续运行及最终同源全平台和正式性能要求保留。

## Latest continuation status (2026-11-13 08:53 UTC)

- Frozen candidate21 hosted run 43383568252 is still in progress. Windows native workerd compatibility passed; the Windows host-side matrix remains active. GitHub reports only the active step; log download returns Azure `BlobNotFound` (HTTP 404).
- Candidate21 ASan remains failed in `websocket_native_reader_lifecycle_managed`; retry of job 131746692045 was rejected by GitHub API with HTTP 403 `Resource not accessible by integration`. No sanitizer diagnostic proves a memory defect; startup ECONNRESET is unresolved.
- Candidate17 unfiltered local TSan CTest completed 1105 tests: 1098 pass, 6 fail, 1 skip, 0 timeout/not-run. Keep this distinct from candidate21 hosted TSan (1006 selected, 1001 pass, 5 skips, 0 fail).
- Candidate17 production managed WebSocket soak passed: 1800.212s, 4274 rounds, 68384 echoes, 2137 deployments/replacements, max host RSS 25516 KiB. Final channels/active/dials/queue/http counters were zero; graceful shutdown code 0. This is stability evidence, not formal performance evidence. Full record: `evidence/v0.3.0/2026-11-13-candidate17-tsan-full-completion-review/review.json` and `/mnt/d/capsid-verification-20261108/evidence/v030-final17-managed-websocket-soak-r1/result.json`.
- The serialized local candidate queue has naturally advanced to candidate18. Do not interrupt or overlap it. Release ordinary WPT still has five known standard failures; workerd compatibility profile is accepted without claiming ordinary-suite green or standards conformance. Formal performance points remain 0, and four-viewport visual acceptance remains open.

> The preceding qualification snapshot is historical (2026-11-13 05:51 UTC, candidate17). The following continuation block is authoritative for later candidate21 and local-queue status.

## 2026-11-13 08:56 UTC update

- The candidate17 production soak has completed and passed with the complete resource/shutdown record in the linked evidence above.
- Candidate18 local queue is building the production target now (`cmake --build ...final18-r1 --parallel 2`); it is an existing serialized controller. Candidate19–21 remain queued behind it.
- Hosted candidate21 Windows host-side CTest is still reported in progress at step 15. Native workerd compatibility passed. GitHub step details have not advanced and its log endpoint returns `BlobNotFound`; no test count or conclusion can yet be independently confirmed.
- Qualification is still open: candidate21 ASan failure has no authorized retry through the connected GitHub integration (HTTP 403); five standard Release WPT failures remain; formal performance evidence is 0; four-viewport acceptance is pending.

## 2026-11-13 08:59 UTC update

The candidate18 production build remains active after approximately 5m37s; the serialized queue controller is still running and no later candidate has started. Hosted candidate21 Windows CTest remains in step 15 (`in_progress`) on the latest status query. No new test result is available. The candidate17 soak has passed as recorded above.

## 2026-11-13 09:00 UTC update

- GitHub job metadata confirms Windows `Run Windows host-side matrix` started at 08:28:29Z and is still `in_progress` at 09:00Z (~31 minutes). The run has no completion timestamp. A successful macOS matrix took ~35 minutes and hosted TSan ~62 minutes, so the Windows duration alone does not prove a hang. Continue polling rather than restart.
- Candidate18 is still in its pinned Release/LTO production build (`strict-all-running`, CMake build PID 989909, parallelism 2); no test matrix for candidate18 has started yet. Its queue controller and later candidate queues remain alive.
- Browser access host is still unavailable (`apps=[]`, `browsers=[]`), so four-viewport visual acceptance cannot be performed in this session.

## 2026-11-13 09:03 UTC update

The candidate18 build is actively compiling: two high-CPU `clang++-18` processes are compiling the frozen candidate18 host sources under Release flags. This confirms forward build progress rather than an idle queue. Candidate21 Windows host-side matrix is still active; current job metadata records step 15 running since 08:28:29Z.

## 2026-11-13 09:04 UTC update

Windows CTest has now run for ~36 minutes. This is within the observed durations of the completed macOS host matrix (~35 minutes) and hosted TSan matrix (~62 minutes), so it remains an active wait rather than a confirmed hang. Candidate18 Release compilation remains live in two compiler processes.

## 2026-11-13 09:06 UTC update

Candidate18 production build log now reports Ninja step 946/1022 (~92.6%), with active compilation continuing. The Windows host-side matrix remains active at ~38 minutes (step 15); no conclusion yet.

## 2026-11-13 09:07 UTC update

Candidate18 Release build has advanced to Ninja step 1004/1022 (~98.2%). Windows host-side matrix remains active at ~39 minutes.

## 2026-11-13 09:09 UTC update

Candidate18 now has an exit-0 Release/LTO build (configure 12s; strict-all 847s). Its unfiltered serialized CTest has registered 1113 cases and is live; the temporary CTest log currently records 273 passed and 0 failed, with more tests still running. Windows candidate21 matrix remains active at ~41 minutes.

## 2026-11-13 09:10 UTC update

Candidate18 unfiltered Release CTest advanced to 298 recorded passes, zero failures so far, of 1113 registered tests. Windows candidate21 host matrix remains active at ~42 minutes.

## 2026-11-13 09:12 UTC update

Candidate18 full unfiltered Release CTest log now records 382 passed, 0 failed out of 1113 registered; the CTest process remains live. Windows candidate21 host matrix remains in progress at ~44 minutes.

## 2026-11-13 09:13 UTC update

Candidate18 full Release CTest log records 392 passes and zero failures so far; it remains active in the 1113-test run. Candidate21 Windows host-side matrix remains active at ~45 minutes, with no result or log available yet.

## 2026-11-13 09:14 UTC update

Candidate18 CTest is still advancing: 395 passes, zero failures. The most recent completed test is `binding_fetch_arguments_raw_reflect` (24.05s); the full registered set remains 1113. Candidate21 Windows matrix is still active (~46 minutes).

## 2026-11-13 09:16 UTC update

Candidate18 CTest log has 399 passed, 0 failed; latest finished case `binding_fetch_arguments_gzip_subclass` took 24s. Windows candidate21 host-side matrix remains active at ~48 minutes.

## 2026-11-13 09:17 UTC update

Candidate18 unfiltered Release CTest has advanced to 409 passes and zero failures, still running. Candidate21 hosted Windows matrix remains in step 15 at ~49 minutes; the job is live and has not produced its conclusion.
