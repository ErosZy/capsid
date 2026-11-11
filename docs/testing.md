# Testing and Continuous Gates

<!-- capsid-current-qualification-20261110 -->
本页后续历轮内容保留原记录身份。当前兼容边界、冻结输入、原始失败和封板开放项见[当前验收与边界](v0.3.0-current-qualification.md)。当前源码版本为 0.3.0 候选，v0.3.0 尚未封板或发布。

部分历史产物已按用户授权清理；[证据索引](evidence/v0.3.0/2026-11-10/index.json)及[可用性审计](evidence/v0.3.0/2026-11-10/historical-evidence-availability.json)说明时点与范围。旧章节的“当前”“仍保留”不表示本轮重新验证了已删除文件。
<!-- /capsid-current-qualification-20261110 -->


本页的验证归档链接指向随源码保存的时点副本；原始位置、SHA-256 和采集时间见[归档索引](evidence/v0.3.0/2026-11-07/index.json)。原始执行日志仍按索引中的本地位置保留，进度以页首最近一轮记录为准。



<!-- capsid-review-phase-107 -->
<a id="capsid-review-phase-107"></a>

## 第107轮（2026-11-07）：SDK70与分支修复合入，继续主线与干净构建验收

**当前主树为 SDK70、JS v11 分支修复，版本仍为 0.2.2；v0.3.0 整体目标尚未完成。** 原先“停在最终组合验收前”的历史边界已被当前完整目标取代。主树没有提交、推送、提升版本或发布；独立干净验证副本中的本地快照提交仅用于满足现有 Release 身份要求。

SDK70 新增 `0070-native-fetch-encoded-branches.patch`，处理压缩分支的结束、失败退场和最后一个消费者释放上游；JS 同步处理已开始解码后的 identity 分支。`js/streams.js` 已由受控 TypeScript 补丁重新生成，生产生成器 `--check` 通过；生成正文与通过验证的私有候选等价。补丁锁定的 overlay key 为 `b9d1f367f166f40117e64d87a4cc3f4a6aa64d9c6b4eb8c0a679adc866bd65bd`，manifest 为 `e27b6acc1a282a92dc39503ca4d059a54b55fb469af88b2fd96e549328d8efd0`。

- 合入依据为封存的私有 R8 验证：三配置合计 1212 个用例（888 个完整公开状态、216 个生产 GC 场景、108 个配额场景），23238 个一致修改反例被拒绝，96 个邻接 CTest、零跳过。它们属于原私有输入；主线刷新单独记录。另有 Linux 原生参数矩阵 390 个完整公开状态匹配、16380 个产品嵌套观测及 4160 个一致状态修改反例。它们没有单独关闭 R3 或旧大 JSON IPC 诊断。
- 首次 SDK70 主线输入冻结为 4774 个文件。Windows Debug 原选集加新增测试为 173 个组合，已有 171 个通过记录；两个完整门禁控制命令在原 120 秒内超时，原始结果与串行复核均保留。Windows ASan 为 156 个不同组合：首次有效运行的 149 个通过、三个失败的原命令串行复核通过、四个 Hono 通过，零跳过。并发运行中出现的两个解码超时和一次自然退出断言失败仍保留，后续完整验收检查稳定性。
- Windows Debug、Windows ASan 的全新构建，以及独立干净 Windows Release 和 Linux Release/LTO 构建均已实际完成。四套产物的 Hono 各四项全部通过，共 16 项，原 500 毫秒应用预算保持不变。干净 Release/LTO 的其他完整矩阵尚待执行。ASan 新构建最初缺少相邻运行库，DLL 加载失败的 152 项原始记录保留；补齐构建记录所指定的三份 MSVC 运行库后重新运行，没有改变产品二进制或测试预算。
- 门禁测试的私有优化版仅把不可变构造模板缓存为序列化字节，每个控制仍获得新的对象图；逐值、共享引用和对象独立性均验证。完整 validator、实际归档 I/O、292 个冻结控制和每组 120 秒时限保持不变。主组 114 项约 70 秒、归档组 117 项约 103 秒、扩展组 61 项约 50 秒，全部通过、零跳过。该优化现已合入，主树正式登记命令的刷新尚待完成。
- 严格 Linux 构建发现新助手的单参数 `static_assert` 不兼容 C++11；已补诊断文字，判断式、5/10 秒事件截止和原有测试预算均不变。该修正在私有 Linux 干净副本编译通过后合入主树；Windows 受影响助手与相关命令继续刷新。此次后续修正只改变两个测试源文件，生产 C++、SDK 和 JS 字节未变。

本轮证据目录为 `build-v030-dev/evidence/fetch-encoded-layer-20261107/`。私有完整依据见 `v11-framed-r8-progress-audit.json`，受控生成与 SDK 身份见 `sdk70-main-integration-r2/integration.json`，测试源码后续修正见 `sdk70-main-integration-r3/integration.json`。每次新输入的构建、命令、原时限、JUnit、失败重跑与源/产物摘要均单列保存；当前需求账本为同目录 `current-goal-requirements.json`。旧 SDK69 的 501 组合及以下第106轮以前的进度保持历史身份，不能算作当前 SDK70 新输入的完整验收。

**继续执行：** 刷新主树受影响测试并完成当前候选的独立干净 Debug/Release/Linux LTO 与不筛选完整矩阵；继续 R3/R5/R6/R7/R8，最后完成 R9。原生参数与分支资源图的其余组合、DefaultReader 精确序列/时延、旧 Linux 大 JSON `invalid IPC frame` 定因、历史15个失败组合和真实跳过前提仍逐项审查。完整依赖 instrumentation、ASan/UBSan/TSan、fuzz/soak、Linux strict、受支持的 Windows/macOS native、真实 Hosted CI、同会话正式三车道五轮性能、真实 2 GiB 密度及四视口仍须实际证据。WSL 功能验证不能充当正式性能验收；正式性能发布点仍为 0，T15/T16/T17 保持开放。

以下保留各轮原始进度和历史边界，以上述当前状态与当前需求账本为准。



<!-- capsid-review-phase-106 -->
<a id="capsid-review-phase-106"></a>

## 第106轮（2026-11-07）：私有上下文与未读gzip分支验证进展

当前主线保持 **JS v5、SDK69、版本0.2.2**。已合入的C++响应退场/poll关闭修复继续持有501个不同“测试×配置”组合的资格：Windows普通169、Windows ASan152、Linux UID1000 180，跳过0。私有JS/SDK实验尚未合入；本轮私有运行不加入主线501的计数。主线依据为 [主线501组合审计](evidence/v0.3.0/2026-11-07/response-retirement-progress.json)，本轮依据为 [私有v9审计](evidence/v0.3.0/2026-11-07/context-v9-progress.json) 和 [私有v10审计](evidence/v0.3.0/2026-11-07/encoded-v10-progress.json)。

私有v9已完成54条动作、36条单读、72条已开始读取分支GC及96条邻接CTest；45条gzip完整状态仍保留切块差异。私有v10的未读tee/clone高熵矩阵三配置144条完整状态全部匹配固定实际参考，消除原30处延迟gzip分支差异；另通过252条内部CPU接口检查、72条未读分支GC及96条邻接CTest。v10的87份邻接协议报告已独立重放。编码pending保活点的实际负对照在三配置复现预期失败，同一变体的raw控制均通过。详细输入、摘要、反例和范围见上述审计。

**v0.3.0整体目标继续进行。** v10当前仅在响应正文限额为0时启用未读gzip分支路径；它明确拒绝“编码子分支已开始读取后再tee”的尚未实现路径。已开始解码后的identity层分支、45条上下文gzip切块差异、限额/畸形压缩流/中断/Binding及更多构造器与并发组合仍需完成；旧684条私有公开用例尚未全部在v10刷新。接下来实现并验证这些范围，再考虑主线JS/SDK集成。R1/R2/R4保持各自定向要求完成，R3/R5/R6/R7/R8开放，R9未完成，详见 [当前目标账本](evidence/v0.3.0/2026-11-07/goal-requirements-at-capture.json)。历史“停在最终组合验收前”的旧边界已被当前完整目标取代。

首版私有方法未转发的4条实际失败、ASan整批CPU检查90秒超时、原始编译/换行检查失败及审计脚本修订均保留。CPU重跑把相同64条断言分为8个互不重叠分组，各组保留90秒界限。历史正文原字节完整备份；[本轮文档变更证明](evidence/v0.3.0/2026-11-07/document-update-proof.json)只允许这9份文档改变，代码和测试资格保持可追溯。本轮未执行commit、push或release。

以下历次记录作为历史保留；当前状态以第106轮和当前目标账本为准。


<!-- capsid-review-phase-105 -->
<a id="capsid-review-phase-105"></a>

## 第105轮更新（2026-11-07）

C++快照退场与poll关闭修复已合入；当前目标选集 **501个不同测试×配置组合通过、跳过0**（Windows普通169、ASan152、Linux180）。JS仍为v5，SDK69、版本0.2.2。主线正式反例21条中8条失败，19条运行器失败及全部修复重跑保留；12条Hono维持500ms应用预算串行通过。本轮主线共执行562条，535通过、27失败，165条私有候选回归单独记录。

私有v8的72条GC资源策略场景及36条单读跨请求错误/closed/release场景通过各自独立模型；gzip18条仍有分段差异，扩展所有权动作54条中51条仍有公开状态差异。v8保持私有，完整GC和分支所有权要求仍开放。快照正式反例中的健康第二请求实际完成，失败在TypeError诊断及退出1。

完整范围和剩余工作见 [当前交接文档第105轮](v0.3.0-next-ai-handoff.md#capsid-review-phase-105) 及 [分支后续计划](v0.3.0-native-fetch-branches-plan.md)；主线审计为 [主线审计](evidence/v0.3.0/2026-11-07/response-retirement-progress.json)，GC审计为 [私有GC审计](evidence/v0.3.0/2026-11-07/lifetime-v8-progress.json)，上下文审计为 [私有上下文审计](evidence/v0.3.0/2026-11-07/context-v7-progress.json)，当前账本为 [当前需求账本](evidence/v0.3.0/2026-11-07/historical-goal-requirements.json)。R3/R5/R6/R7/R8继续、R9未完成；当前完整v0.3.0目标取代历史“停在最终组合验收前”的边界。未commit/push/release或修改版本，历史正文按原字节保留。


<!-- capsid-review-phase-104-in-progress -->
<a id="capsid-review-phase-104-in-progress"></a>

## 第104轮进行中（2026-10-24）：主树480个定向组合，补齐4处助手额度并定位运行时超时异常

当前主树定向检查点为 **480 个不同“测试×配置”组合**：Windows 普通162、ASan145、Linux UID1000 173，当前资格结果失败/跳过0/0。审计为 `upgrade58bsj-main69-helper-credit-progress-audit.json`。本轮刷新18个受助手产物变化影响的旧组合，首次资格化6个已有协议/取消命令组合，新增18个正式组合；另438个在核对源文件、产物和注册命令未受影响后保留。测试输入冻结共4755份，三配置各跟踪25个产物；主树Worker仍为v5，SDK69、版本0.2.2保持当前身份。

0.3.0整体继续进行。R3/R5/R6/R7/R8开放，R9未完成，第104轮未封存。**主树新发现的快照正文超时/取消异常仍未修复；下面的C++修复仅在私有副本验证。** 最终组合、全量/clean、真实CI、平台/性能/密度/viewport验收仍留在约定边界之后；没有提交、推送、发布或改版本。

### 已合入主树：4处C API测试助手归还响应额度

`tests/test_native_fetch_deadline.cc`、`tests/test_fetch_response_limits.cc`、`tests/test_worker_channel_discard.cc`、`tests/test_worker_v4_negotiation.cc` 现在通过 `tests/response_credit_probe.h`，按每次实际消费的 `RESPONSE_BODY` 字节调用 `capsid_worker_grant_response_credit`。该API只把额度帧排入本地队列，不主动flush，保留channel-discard先观察CANCEL、再发送暂存DATA与ACK的屏障顺序。原调用参数、期限和协议断言保留；显式 `--small-response-window` 模式补上跨窗口观测。

| 新命令后缀（统一前缀helper_response_credit_） | 初始响应窗口 | 实际覆盖 | CTest期限 |
| --- | ---: | --- | ---: |
| deadline | 32字节 | 31/32/33/101字节正文，各带健康第二请求 | 30秒 |
| limits / limits_encoded | 256字节 | 每项保留320个原限额、分片、取消、encoded-header场景 | 各60秒 |
| discard | 16字节 | 12种取消交叉及12次健康请求复用 | 30秒 |
| negotiation | 3字节 | 保留7组capabilities及未知选项/v3 READY检查 | 30秒 |
| contract_controls | — | 完整字节、额度、顺序、响应头、完成和诊断反向控制 | 30秒 |

先冻结无额度修正的原行为并运行真实CTest：三配置15个命令失败，24个原始观测中18个耗尽初始窗口、6个小正文虽完成却没有归还额度；完整stdout/stderr、JUnit、LastTest及旧助手产物保留在 `upgrade58brv-negative-*`、`upgrade58brw-no-credit-artifacts-*`。修正后新增18个命令组合全部通过，15份完整报告共24个观测，逐字节核对 **2037个响应、1251210字节、6108个实际分片，其中1983个响应超过初始窗口**。另复核默认命令的1920条限额场景，90条deadline场景的完整语义状态与前次一致。

新增校验器最初把所有响应头都当成fixed-body，导致3次真实CTest审校失败。源码规则是：正文不超过初始credit时为fixed-body，否则必须是流式头，flags=0、credit=0。修正采用这个确定规则，新增24个元数据反例，controls每配置拒绝109份错误记录；保存的24个修正后观测全部通过，24个无额度旧观测仍全部拒绝。3次错误预期的原始失败没有覆盖成通过。独立复核见 `upgrade58bry-model-replay.json`、`upgrade58bsj-full-response-review.json`。

本轮正式CTest共75次：57次成功、15次无额度反例失败、3次响应头模型错误失败。最终资格仅保留42次成功命令，另15次已被更正模型后的执行替代。SDK69历次选集累计1526次：1466次成功、60次历史失败保留，986次成功已被后续运行替代；1466−986=480。私有直接执行和模型断言不计为额外CTest组合。

前轮 `tests/test_native_fetch_response_limits.cc` 的默认256KiB消费额度修正也继续保留，四个实际响应262143/262144/262145/786469字节、三配置12个响应及每配置57个controls仍按其原证据范围计入当前选集，见 `upgrade58brh-main69-credit-progress-audit.json`。四处后续助手任务现已实现；不能据此宣称所有C API消费者或运行时错误路径已完善。

### 新发现：快照正文在部分发送后超时/取消会污染运行时异常状态

原生快照写入的 `PendingWrite.resolve/reject` 是 `JS_UNDEFINED`，但 `resolve_pending/reject_pending` 仍直接调用。超时或取消时会产生 `TypeError: not a function`；旧5秒超时观测还出现过Windows ASan的 `UV_HANDLE_CLOSING` 断言。源码另显示shutdown关闭poll后，后续输出路径仍可能调用update_poll。两条路径的关联仍须独立隔离，不能仅凭症状消失宣称关闭句柄问题的因果证明完整。

私有C++候选在 `upgrade58bsa-candidate/worker_runtime.cc`，只跳过不存在的快照回调，并防止重新启动关闭中的poll；真实Promise回调的请求上下文、释放与原处理方式保留。候选使用 **主树JS v5**，没有混入私有JS v7，也未改SDK69。三套编译/链接产物分别在 `upgrade58bsb-{native,native-asan}-private/` 和 `upgrade58bsc-linux-private/`，主树25个产物未改变。

独立C API复现使用32字节窗口、33/101字节正文、750毫秒请求期限，覆盖快照/流式正文 × 超时/取消/归还额度/持有写入时关闭。三配置原产物36项通过、**12项快照超时/取消真实失败**；候选48项全部通过完整事件/字节、超时终态、同一Worker健康复用及自然退出模型，576份一致改写反例全部拒绝。Windows ASan的READY按其实际零process-memory默认值复核；最初错误的私有模型判定也保留，见 `upgrade58bsd-windows-replay.json`。

另用字节相同的旧5秒无额度助手重复12次：原产物新增复现1次关闭句柄断言；候选6次没有TypeError或该断言。但助手自身5秒事件期限会与Worker的5秒期限竞态，**这12次助手失败仍是失败**，不能当成正确终态或健康复用证据。完整语义证明来自独立750毫秒探针。汇总为 `upgrade58bsk-private-retirement-and-v7-audit.json`。C++候选尚未注册正式CTest、完成相邻全选集回归或合入主树；可审阅的未应用补丁与具体步骤见 `upgrade58bsl-runtime-candidate.patch`、`upgrade58bsl-runtime-integration-proposal.json`。

### 私有JS v7仍按原范围保留

v6有12个gzip低限额场景丢掉共享identity队列中已接受的字节；v7只在限额退场前将这些字节发布到独立分支，再报告同一额度异常。19份私有源文件及三套产物仍不可变，输入为 `upgrade58brj-candidate/js/`。v7先读后分支144、动作/嵌套288、未读再分支144，共576个与两套固定workerd实际参考比较的观测中546个完整匹配，**30个未开始读取的gzip分段差异仍开放**；C API限额108个另按局部策略模型验证。684个观测均完成并自然退出，4512份一致改写反例被拒绝。限额夹具优化前23次ASan期限失败、v6的12次丢数据失败、原夹具56次参考HTTP500及3次中止采集均保留。

原25条注册命令/配置的私有Worker替换回归，现在按63条未受影响命令保留、12条deadline命令用新额度助手刷新。90条刷新场景状态与旧记录一致，当前仍为75条命令、1077条场景；历史执行累计87次，不增加主树480组合。证据为 `upgrade58bse-existing-*.json` 和 `upgrade58bsk-private-retirement-and-v7-audit.json`，原完整分支证明仍为 `upgrade58brp-private-v7-progress-audit.json`。v7尚未合入或注册正式测试。

### 当前差异与继续顺序

先把C++超时/取消复现整理为正式fixture/独立contract/反例，补齐关闭poll的独立验证及相邻回归；满足条件后合入精确补丁，刷新所有受运行时变化影响的主树组合。**不要把当前480个旧运行时组合直接搬到修改后的Worker。** 然后继续v7的分支GC、跨请求所有权/并发、values/pipe和任意传输分片额度/异常退场，最后正式化并评估JS候选合入。

主树v5仍保留72个gzip分段差异观测：未读正文30个、先读后分支42个。v7在已测量范围内解决后42个，前30个需要另做编码层分支原型并与实际参考比较。固定参考为workerd v1.20261010.1、提交8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f、兼容日期2026-08-20、flags=[]；完整状态直接比较。新C API额度/请求期限探针没有对应的公开workerd配置等价承诺。

详细方案见 `docs/v0.3.0-native-fetch-branches-plan.md`。证据统一在 `build-v030-dev/evidence/native-asan-qualification-20261018/`：主树输入/构建/失败/最终新增执行依次为 `upgrade58bry-project-inputs.json`、`upgrade58bru-*-credit.json`、`upgrade58brv-negative-*`与`upgrade58brx-*`、`upgrade58brz-*`；主树审计为 `upgrade58bsj-main69-helper-credit-progress-audit.json`，私有审计为 `upgrade58bsk-private-retirement-and-v7-audit.json`。源清单与文档更新后的清单分开保存；以下phase103及更早内容按原字节保留，其历史待办不替代本轮结论。

<!-- capsid-review-phase-103 -->
<a id="capsid-review-phase-103"></a>

## 第 103 轮（2026-10-21）：BYOB 参数顺序、标准容量拒绝与 ABI 自检同步

本轮关闭此前已证实的 **8 项标准 JS 容量拒绝差异**，并补上五种构造路径的参数转换、视图、释放状态和接收者检查。锁定 Windows、Linux workerd **v1.20261010.1**（提交 `8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f`）各 **435/435**；修复前的实际 phase102 产品各 **417 通过、18 失败**，原 417 项全部保留通过。新增失败恰为 8 项标准容量场景和 10 项验证组，原始记录 `upgrade57e-*`、独立复核 `upgrade57f-baseline-review.json` 保留。参考兼容日期仍为 **2026-08-20**，flags 为空；完整锁定提交为 `8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f`。

产品修改集中在 `js/websocket.js`：严格校验 BYOB reader 的实例身份；区分 `read(view, options)` 与 `readAtLeast(min, view)` 的参数绑定顺序；`min` getter 与转换遵循短路顺序，在应执行的路径上各执行一次；转换抛出的原对象保持身份。按参考先检查有符号 32 位范围，再截断小数；BigInt、Symbol、非法视图、已分离输入、已释放 reader、转换期间释放或分离等已测组合要求相同异常类型、文字与顺序。标准 JS 流在验证后委托原有读取，保持其视图类型；原生路径继续按元素宽度转换字节 minimum。释放后重新获取的 reader 必须仍能完整读取原来的 16 字节数据，再完成 EOF、取消及解锁，防止参数错误吞掉数据或污染状态。

正式清单为 **435 = 330 项原生视图 + 64 项标准 JS 对照 + 10 项类型借用 + 10 项参数验证组 + 21 项原生 body**。10 组含 **420 条**独立观测：五种构造路径各有 read 的 44 条和 readAtLeast 的 40 条，检查事件顺序、getter 次数、异常原对象及同 VM 类型、输入分离、backing、后续读取和收尾。这 420 条使用标准 JS 字节源，每次完整排队 16 字节，视图主要为 Float64Array，另含空、已分离和无效输入；真实 fetch 原生 body 的同规模参数及错误后重试/资源矩阵仍待补充，其他视图族也须另测。内部观测不冒充新增 CTest。原 417 项的相对次序、独立 46 项 transition、21 项大体积/空体/短体 body 全部保留。五个运行时分组为 **100、100、106、108、21**，每组 **120 秒**、每场景 peer **10 秒**不变。正式契约和反例控制 **8507/8507**；包括参考与所有合成候选同时出现相同容量错误、getter 次数或异常身份错误时仍必须拒绝的独立控制。完整门禁原有 **482** 项控制继续按 **114 + 114 + 254** 执行，冻结的 228 项名称、全部实际执行名称、精确命令/环境/时限均未放宽。三组各 120 秒、合计预算 360 秒沿用第 102 轮，本轮没有再增加。

第一版产品的重点验证在普通 Windows 和 ASan 各有 **10 个验证组失败、20 条嵌套观测失败**，均为负值 unsigned64 诊断。实际普通 Windows、Linux 产品的全局 `BigInt.asUintN(64, negative)` 返回负数，而参考返回对应的无符号整数；原始探针和失败保留在 `upgrade57j-*`、`upgrade57j-first-focus-review.json`。本轮仅在 BYOB 路径改用精确算术 `(1n << 64n) + negative`，**没有修复全局 BigInt 内建函数**。最终三配置的第 4、5 组重点测试均通过，记录为 `upgrade57l-*`；旧 `upgrade57h-*` 继续保持失败。

完整回归的首轮为 **223/224**：普通 Windows 的 `abi_guard_without_worker` 在私有控制子进程已确认退出且句柄已关闭后，立即查询作业统计仍读到 2 个进程；ASan 和 Linux 回归全部通过。原始 `upgrade57m-native.json`、JUnit 和完整日志保持失败。随后只修改 `tests/test_abi_guard.cc` 的这段自检：分别要求真实进程退出、退出码为 0、最多 5 秒内作业计数回到精确的 1；新旧检查都必须拒绝真实活子进程，通用最终清理检查仍即时执行。Windows 作业统计在进程退出并释放引用后更新，依据 [Microsoft 作业统计说明](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information) 与 [TerminateProcess 说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)。原版和候选版的独立抽取在两个 Windows 配置各跑 100 次均通过，独立抽取没有稳定复现原失败，不能代替完整测试。修正后的完整 ABI 可执行程序在普通 Windows、ASan 各连续执行 **50 次**通过；这 100 次重复执行仍只对应一个注册测试，不计入下面 224 项。

| 最终主构建 | ALL / 打包 | 修正 ABI 自检后新执行的回归 | 本轮实际执行并经严格复核的完整门禁 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 74/74 | 79/79 |
| Windows ASan Debug | 通过 | 74/74 | 79/79 |
| Linux UID 1000，构建类型为空 | 通过 | 76/76 | 100/100 |

最终回归 **224/224**，完整必需门禁 **258/258**，均零失败、零跳过。门禁在上述 ABI 测试文件修正之前、已经是最终运行时的 `upgrade57k` 输入上执行；后续 `upgrade57t` 的 ALL/打包确认 **host/worker 及其他所有门禁产物逐字节一致、全部 CTest 注册完全相同、4519 份输入仅 ABI 测试源码一处变化**，据 `upgrade57t-retained-runtime-gates.json` 保留门禁。最终源输入的回归是新执行的，门禁则明确沿用本轮同产物同输入的实际执行，**不宣称 ABI 自检改动后重新执行过全部门禁，也没有复用上一轮 phase102 门禁**。最初普通 Windows 的回归失败并未被门禁通过覆盖。CTest 注册仍为 **577 + 578 + 811 = 1966**；本轮未执行不筛选的全部 1966 项。

证据根目录为 `build-v030-dev/evidence/native-asan-qualification-20261018`。运行时门禁冻结输入与完整快照为 `upgrade57k-project-inputs.json`、`upgrade57n-tested-project-inputs`；最终 ABI 修正输入与快照为 `upgrade57t-project-inputs.json`、`upgrade57v-tested-project-inputs`，均 **4519** 份。最终 ALL/打包为 `upgrade57t-build-*`，新回归为 `upgrade57u-*`，完整门禁为 `upgrade57m-*`，未截断门禁日志为 `upgrade57o-*`，严格归档及旧产品失败重放为 `upgrade57p-*`。最终 **418** 份产品、库、对象及选定诊断文件在 `upgrade57v-current-build-binaries`。封存为 `upgrade57-final-audit.json`；八份文档在验证后更新，由 `upgrade57w-post-verification-documents.json` 单列。产品文件一份、测试/契约/样本八份、文档八份共 **17** 份修改，无新增或删除文件。

**body 强兼容与 0.3.0 仍未整体完成。**此前 44 个未资格化标准组合中的 8 个容量场景已加入本轮正式通过，余下 **36** 个继续分开追踪：12 个非对齐 EOF 内部错误、6 个 Float64 零长度 EOF 视图、10 个受控高 minimum 取消、8 个旧等待脚本问题。额外私有探针在两平台各 **59/59** 条 16 字节源观测相同；8 字节源各 **44/46** 条相同，仍有 read/readAtLeast 两项 Float64、minimum=2.9（截断为 2 个元素，需 16 字节）在对齐的 8 字节尾块处不一致：参考返回该尾块，产品报 `Insufficient bytes to fill elements in the given buffer` 并影响后续收尾。原始差异和完整比较见 `upgrade57q-postfix-diagnostic-review.json`，不因 420 条完整 16 字节源参数检查通过而关闭。

真实原生 body 的参数/释放/无效输入后重试及资源矩阵、全局 BigInt 无符号转换、其他 coercion/receiver/intrinsic 变更、并发读取、owner/abort/context end 和 released.closed 等语义继续开放。实际非零 C ABI `max_fetch_response_body_bytes` 的 Upgrade body 路径、已知/未知长度 below/equal/above 及错误回收仍未测试。DefaultReader 精确 chunk 序列、时延/性能和旧 Linux 大 JSON `invalid IPC frame` 仍未关闭；ordinary fetch metadata 与 tee/clone/values/pipe 资源图也未整体资格化。历史 **15 个失败组合**、最终输入不筛选 1966 项、独立干净 Debug/Release/Linux LTO、真实 CI、完整依赖 sanitizer/UBSan/TSan/fuzz/soak、正式三车道五轮性能、真实 2 GiB 密度及四视口仍待验收。正式性能发布点为 **0**，T15/T16/T17、D005/D010/D012 开放。版本 **0.2.2**、补丁锁 **59**、Root HEAD 均不变，没有提交、推送或发布。

下一步见 [第 103 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-103)。以下第 102 轮及更早章节按历史输入保留，待办状态以本轮结论为准。

<!-- capsid-review-phase-102 -->
<a id="capsid-review-phase-102"></a>

## 第 102 轮（2026-10-21）：大体积读取、DefaultReader 分配及偏移 EOF

本轮补上超过初始窗口的实际读取，并修正两处真实参考差异。锁定 Windows、Linux workerd 各 **417/417**；修复前两平台产品各 **401 通过、16 失败**，其中 **6 项 DefaultReader backing 容量**和 **10 项非零输入偏移的 BYOB 空 EOF 偏移**。原始对照 `upgrade56e-*`、逐项审阅 `upgrade56f-baseline-case-review.json` 保留。最初校验器把 EOF 偏移误写为 16，真实参考均返回 0；当时的参考失败退出未改写，修正期望后的新采样 `upgrade56g-*` 两平台各 417 项实际通过。

最终产品只改 `js/websocket.js`。原生字节源以 **4096** 字节自动分配 DefaultReader 缓冲区，向控制器提供的 BYOB 视图复制数据并 `respond()`；当视图容不下整个传输碎片时保留剩余部分，按实际复制字节数返还信用。仅配置自动分配再 `enqueue()` 会继续暴露原始碎片，不能完成这项修复。原生 BYOB 空 EOF 返回同一已转移 backing 上、偏移 **0** 的零长度字节视图，保留 backing 容量及外侧填充值。限制包装层转交已有的正确 backing，最终没有修改 `js/fetch.js`。

正式清单为 **417 = 330 项原生视图 + 56 项标准 JS 对照 + 10 项类型借用 + 21 项原生 body**，原 396 项及独立 46 项 transition 全部保留。新增 21 项包括五种构造路径的 DefaultReader 空体/短体 10 项、五种路径的 BYOB minimum/readAtLeast 10 项，以及 getReader 的大体积 DefaultReader 1 项；所有大体积响应均为 **1 MiB + 37 字节**。BYOB 单次 minimum 要求完整响应，输入偏移 16、两次输入分离、空 EOF、完整顺序字节与 SHA、外侧字节、同 VM 身份和正常收尾全部检查，实际跨越 **64 KiB** 初始窗口。

每个实际参考/后端执行 **100、100、106、90、21** 五组，门禁要求全组、完整清单、同一输入/二进制/run 身份及跨组独立 challenge/VM。每组仍为 **120 秒**，单场景 peer 仍为 **10 秒**。原始 body 传输记录无损压缩，解压后核对原始长度、SHA 和每个字节，不裁剪大响应。观测器用等价的 32 位批量读取减少 JS 回调开销；另有 **111150 次外侧区间 + 53240 次 payload = 164390 次**独立逐字节边界对照，覆盖空区间、非对齐首尾、每个单字节改动及多个错误。正式字节流契约/反例控制 **7733/7733**。原六文件门禁控制仍为 **482/482**，按 **114 + 114 + 254** 分为三个必需 CTest；每组为 **120 秒**，控制组总预算由原先单项 120 秒变为 **3 × 120 = 360 秒**。这项分组调整不代表控制总耗时仍低于 120 秒；运行时每个场景的 10 秒及各 shard 的 120 秒单列约束。前两组每次独立核对冻结的完整 **228 项**有序名称，构建注册和两个审计器同时约束选组环境、精确命令及超时。三组合并后的实际执行名称与原 482 项逐项一致，零跳过；没有采用私有解码缓存方案。

| 最终输入的实际主构建 | ALL / 打包 | ABI、策略、上下文、生命周期与包回归 | 新执行的完整必需门禁 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 74/74 | 79/79 |
| Windows ASan Debug | 通过 | 74/74 | 79/79 |
| Linux UID 1000，构建类型为空 | 通过 | 76/76 | 100/100 |

最终回归 **224/224**、新执行的完整必需门禁 **258/258**，零失败、零跳过；三套最新产物另各通过新增 21 项聚焦测试。没有复用上一轮运行时门禁。CTest 注册为 **577 + 578 + 811 = 1966**；本轮没有执行不筛选的全部 1966 项，内部场景与校验器断言不冒充 CTest 数量。

第一版完整主验证的 224 项回归全部通过，ASan 77 项及 Linux 98 项门禁通过，但普通 Windows 的 77 项门禁有 1 项六文件控制触及 120 秒；原始失败 `upgrade56x-native-initial-failure-review.json` 及 JUnit、完整日志保留。私有解码缓存尝试仍耗时约 110.8 秒，余量不足，未写入产品或正式校验器；最终采用固定清单拆分控制组。新增两个必需 CTest 后，以上最终 258 项门禁来自新输入、新执行，未把先前通过的归档拼接成成功。拆分后的首次聚焦验证另有输出目录遗漏，四项支持控制按实际 ENOENT 失败；修复验证脚本目录创建后已复验，记录为 `upgrade56zh-focus-driver-correction.json`，测试源码未因此改动。

开发期失败均单独保留：第一版普通聚焦因分配未生效而失败，ASan 的 111 项组合触及 120 秒（`upgrade56i-*`）；修正数据写入后，第二版普通聚焦通过，ASan 在约 98 秒退出，因为末尾大体积 DefaultReader 观测触及单场景 10 秒（`upgrade56p-*`）。后者只优化逐字节观测的执行方式，没有缩小响应、减少检查或延长时限；实际产品二进制与最终观测版本核对相同。对应输入分别完整保存在 `upgrade56k-tested-project-inputs`、`upgrade56t-tested-project-inputs`。最早私有探针的 1000 次循环上限与 Linux 大报告 `invalid IPC frame` 记录在 `upgrade56b-*`，紧凑输出后的完整读取在 `upgrade56c-*`；大报告触发的 IPC 诊断尚未单独定因，不列为已修复。

最终冻结输入 **4519** 份：`upgrade56zh-project-inputs.json`、`upgrade56zj-tested-project-inputs`；实际 ALL/打包为 `upgrade56zh-build-*`，实际回归/完整门禁为 `upgrade56zi-*`，未截断门禁日志为 `upgrade56zk-*`，全部实际归档严格重放与旧 16 项失败复核为 `upgrade56zl-gate-archive-replay.json`。最终 **418** 份实际产品、库、对象及选定诊断文件在 `upgrade56zj-current-build-binaries`。封存入口为 `upgrade56-final-audit.json`。这八份文档在验证完成后另行更新，并以 `upgrade56zm-post-verification-documents.json` 区分于测试冻结输入。

**仍未整体关闭 body 强兼容或 0.3.0。**本轮对齐 DefaultReader 固定 backing 容量、字节、偏移与 EOF；实际每块长度和次数仍有差异：本轮聚焦样本的 1 MiB + 37 字节响应，两平台参考各 **257 次**数据读取，当前三套 single-worker 各 **2049 次**，backing 均为 4096。原始形状保留在 `upgrade56yc-default-segmentation-review.json`，完整门禁的各后端序列也由归档重放单列，**没有把精确 chunk 序列一致计作通过**。实际配置非零响应限制的 below/equal/above、已知/未知长度及错误收尾仍未资格化；本轮 runner 没有设置该 C ABI 限额，不能拿大 body 成功代替限制边界通过。BYOB 完全空体、其他 min 转换/参数顺序、无效/已分离视图、同类 reader 释放/重新获取、并发读取、owner/abort/context end、内建属性变更、其他接收者及普通 fetch/tee/clone/values/pipe 分支图仍须补齐。

此前 **44 个标准 JS 字节流组合仍未资格化**：8 项正常容量拒绝、12 项非对齐 EOF 内部错误、6 项 Float64 EOF 零长度视图、10 项受控高 minimum 取消和 8 项旧等待脚本问题继续分开追踪，保留真实错误 ID。其中 8 项容量拒绝已用当前 Windows、Linux 产品重新实测：参考返回 `TypeError: Minimum bytes to read (24) exceeds size of buffer (16).`，产品返回 `RangeError: options.min must be less than or equal to view's length`；原始采样及双平台审阅在 `upgrade56yd-*`，仍不计入正式 417 项通过。其余字段、未分离输入及收尾一致不能代替异常兼容。本轮也没有关闭历史 **15 个失败组合**和跳过前提、新输入的独立干净 Debug/Release/Linux LTO、真实 CI、完整依赖 sanitizer/fuzz/soak、正式三车道五轮性能、真实 2 GiB 密度及四视口。正式性能发布点仍为 **0**；观测器提速不计为产品性能发布点，T15/T16/T17、D005/D010/D012 继续开放。版本 **0.2.2**、补丁锁 **59** 不变，没有 Root 提交、推送或发布。

下一步见 [第 102 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-102)。以下第 101 轮及更早章节按历史输入保留，其待办状态以本轮结论为准。

<!-- capsid-review-phase-101 -->
<a id="capsid-review-phase-101"></a>

## 第 101 轮（2026-10-21）：读取器原型借用、Float16Array 与 ABI 测试归属修复

本轮完成上一轮的两个优先审阅项。真实 Windows、Linux workerd 对照均确认：旧产品在借用另一类读取器的 `cancel()` 时，会先污染挂起读取的取消原因，再拒绝错误接收者；`Float16Array` 则因元素宽度表缺项而错误拒绝正常读取。修复前两平台各 **376 通过、20 失败 / 396 项**，恰为 **10 项原型借用与 10 项 Float16Array**；两平台锁定参考各 **396/396**。原始失败退出、逐项原因和二进制身份保留在 `upgrade55e-*`、`upgrade55f-baseline-case-review.json`，未覆盖为通过。

产品只修改 `js/websocket.js`：用独立的弱引用类型记录，在参数读取、上下文检查和挂起状态修改之前，拒绝 Default/BYOB 读取器之间的跨类原型借用。类型记录在 `releaseLock()` 后继续有效，活动 I/O 上下文仍照常解除关联；错误的 `read/readAtLeast/closed/releaseLock/cancel` 同步抛出固定参考的 `Illegal invocation` 异常，不触发选项 getter、不分离输入，也不改变后续正常取消原因。元素宽度由启动时捕获的 TypedArray `byteLength/length` getter 推导，覆盖 `Float16Array`，不依赖应用可改的构造器或宽度属性。本轮不宣称普通对象、伪造实例和 Proxy 等所有错误接收者边界已经完成。

正式 BYOB 清单由 **306 增至 396 项**，保留旧 **250 项原生、56 项标准 JS 对照**及独立的 **46 项 transition**。新增 `Int8Array/Uint8ClampedArray/Int16Array/Uint32Array/Float16Array/Float32Array/BigInt64Array/BigUint64Array` 八种视图，各覆盖默认/min2/min3/readAtLeast2/readAtLeast3 与 held/EOF，合计 **80 项**，均走 `getReader`；原有五视图的五种构造路径矩阵保留。另增 **10 项原生读取器类型检查**，覆盖两类读取器与五种构造方式，并分别检查释放前、释放后的借用行为。当前共 **330 项原生视图、56 项标准对照、10 项原型借用**；原生视图族共 **13 种（含 DataView）**，不等于十三种视图都跑过五种构造方式。

字面契约及拒绝错误报告的独立控制 **7058/7058**；实际完整六文件门禁控制 **477/477**，其中总门禁控制单文件 **223/223**。新增“所有报告同样错误”的 Float16 分离、取消原因、同步异常和释放后检查反例，并要求新增分组报告实际存在。合成后端报告仅用于校验器控制；产品通过来自以下实际 CTest。

| 最终输入的实际当前主构建 | ALL / 打包 | ABI、策略、上下文、生命周期与包回归 | 完整必需门禁 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 74/74 | 74/74 |
| Windows ASan Debug | 通过 | 74/74 | 74/74 |
| Linux UID 1000，构建类型为空 | 通过 | 76/76 | 94/94 |

最终完整回归 **224/224**，完整必需门禁 **242/242**，最终结果零失败、零跳过。门禁先在 `upgrade55i` 实际产品执行；后续只修正 ABI 测试源码，最终 `upgrade55r` 的门禁脚本/场景与运行产品二进制核对未变，因此按 `upgrade55t-gate-reuse-review.json` 沿用同一实际门禁，并经严格归档重放确认。224 回归则在最终构建完整重新执行，不能把门禁描述成修正 ABI 测试后的新一轮执行。每个实际参考/后端分别执行 **100、100、106、90** 四组，要求完整清单、相同输入与二进制/run 身份，以及跨组唯一 challenge/VM；每项仍保持 **120 秒** 上限。当前 CTest 注册 **572 + 573 + 805 = 1950**，内部场景和校验器断言不另冒充 CTest 数量。

首次完整回归为 **223/224**，其中 ASan **73/74**；失败 `abi_guard_oom_egress_empty` 把三天前启动的无关更新进程当作未收尾子进程。原测试只比较父 PID，历史 PID 复用导致误报。记录 `upgrade55p-abi-ownership-root-cause.json` 保留实际进程身份及来源推断、10 次真实私有 Job 的 1→2→1 转换和普通/ASan 四次独立副本检查；这些诊断不替代正式回归。`tests/test_abi_guard.cc` 现按测试既有私有 Job 的进程计数检查整个子进程树，并在既有 Windows ABI 场景加入真实挂起子进程的拒绝、终止收尾后的通过控制；检查强度与原测试超时未放宽。关于 Job 计数语义参见 [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information)。原始首次失败 `upgrade55j-native-asan-*` 仍为失败；修正后实际完整回归在 `upgrade55s-*`。

初次输入与运行在 `upgrade55h-*`；LF 与样本分组元信息清理后的 `upgrade55i` 输入和实际门禁保留。ABI 测试修正后最终 `upgrade55r-project-inputs.json` 仅再改变 `tests/test_abi_guard.cc`，三套 ALL/打包及 224 回归重新执行，产品源码与门禁运行产品未变。最终 **4518** 份输入在 `upgrade55t-tested-project-inputs`，之前准确捕获输入在 `upgrade55e-baseline-project-inputs`，`upgrade55i` 输入另存于 `upgrade55k-tested-project-inputs`；最终 **418** 份实际产品、库、对象及选定诊断文件在 `upgrade55t-current-build-binaries`。实际回归与门禁记录分属 `upgrade55s-*`、`upgrade55j-*`，完整门禁日志为 `upgrade55l-*`，复用核对与严格重放为 `upgrade55t-gate-reuse-review.json`、`upgrade55v-gate-archive-replay.json`；封存为 `upgrade55-final-audit.json`。这八份文档在验证后单列更新，与测试冻结输入区分保存。

**0.3.0 与 body 强兼容仍未整体完成。**此前 **44 个 JS 字节流组合仍未资格化**；8 项正常容量校验、12 项非对齐 EOF 内部错误、6 项 Float64 EOF 零长度视图、10 项受控高 minimum 取消和 8 项旧脚本等待错误继续分开追踪。原始错误引用 ID 保留，不归一化成通过。未预读首读、空 EOF、min 转换与参数顺序、无效/已分离 view、同类读取器释放/重新获取、并发 BYOB、owner/abort/context end、内建属性变更、超过 credit window 的大 body 与实际响应限制边界仍须补测。普通 fetch 与 tee/clone/values/pipe 分支图继续开放；当前六字节 peer 不能证明大 body 的 credit 补充。 另有实际默认读取形状差异：两平台私有六字节首读中，参考 backing 为 4096、旧产品为 6（`upgrade55b-*`）；本轮原型借用场景把首读作为字节检查点，不校验 DefaultReader 的 backing 容量，不能据此宣称该差异已关闭。后续须与大 body 的分块/分配形状一起补测，详见 `upgrade55l-open-coverage-review.json`。

本轮未执行不筛选的 **1950 项完整矩阵**，旧 **15 个失败组合**及历史跳过前提未关闭；新输入的独立干净 Debug/Release/Linux LTO、真实 CI、完整依赖 sanitizer/fuzz/soak、正式性能五轮、真实 2 GiB 密度和四视口仍待验收。正式性能发布点仍为 0，T15/T16/T17 继续开放。版本保持 **0.2.2**、补丁锁 **59**；没有 Root 提交、推送、版本提升或发布。

下一步见 [第 101 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-101)。以下第 100 轮及更早章节按历史输入保留，其待办状态以本轮结论为准。

<!-- capsid-review-phase-100 -->
<a id="capsid-review-phase-100"></a>

## 第 100 轮（2026-10-21）：Upgrade 回退 HTTP 响应体的原生 BYOB 基础矩阵

本轮修改 `js/websocket.js`、`js/fetch.js`。Upgrade 回退 HTTP 数据通道现在提供字节源；原生 BYOB 读取按原视图的元素宽度换算 minimum，并返回同范围的 `Uint8Array`，保留输入偏移、backing buffer 和分离行为。非空的不足 minimum EOF 尾块保持 `done=false`，原生 `closed` 的正常完成延后到空 EOF 读取或取消。关闭字节源时显式完成挂起 BYOB 请求；字节入队前保存 ACK 长度，限制包装保留已关联原生流的字节能力。标准 JS 字节流保留其视图类型，并增补 `readAtLeast()` 的正向调用路径。

新增独立正式套件 `tests/test_websocket_fetch_byob.mjs`、工作脚本、真实 TLS peer、字面契约及压缩的 Linux 参考样本。每个实际参考/后端执行 **306 项**：**250 项原生**覆盖 `getReader`、直接构造、`prototype.constructor`、子类、`Reflect.construct` 五路径，`Uint8Array/Uint16Array/Int32Array/Float64Array/DataView` 五种视图、默认/min2/min3/readAtLeast2/readAtLeast3 和 held/EOF；另有 **56 项标准 JS 字节流对照**。minimum 为元素数，实际五字节可返回奇数长度，偏移 8、backing 32、分离状态、EOF 空视图、取消原因类型与反应顺序均有独立校验。相同读取器先实际读到前导字节，再测剩余五字节，避免把传输尚未就绪误判为 minimum 行为。旧 **46 项 transition** 全部保留。DataView 已分离后的通用引擎异常检查 `TypeError` 类；本轮不声明 V8/QuickJS 的该通用异常文本完全一致。

真实修复前基线已保留：Windows、Linux 的锁定 workerd 各完成 **306/306**；第 99 轮实际产品逐项为 **36 通过、270 失败**，其中 **250 项原生 BYOB 获取失败、20 项标准流缺少 readAtLeast**。捕获在 `upgrade54e-*`，产品二进制身份核对到上一轮 ALL 产物，审阅 `upgrade54o-baseline-case-review.json`。`tests/test_workerd_fetch_byob_contract.mjs` 独立控制 **5208/5208**；完整六文件门禁控制 **472/472**（其中 `tests/test_workerd_reference_gate.mjs` 单文件为 **218/218**）。合成平台/后端报告仅用于检验校验器，实际产品通过来自后述真实 CTest。

| 本轮实际当前主构建 | ALL / 打包 | ABI、策略、上下文、生命周期与包回归 | 完整必需门禁 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 74/74 | 71/71 |
| Windows ASan Debug | 通过 | 74/74 | 71/71 |
| Linux UID 1000，构建类型为空 | 通过 | 76/76 | 90/90 |

本轮回归 **224/224**、完整门禁 **232/232**，零失败、零跳过，均为修改后的实际主构建重新执行。BYOB 每个参考/后端固定拆为 **100、100、106** 三组；门禁要求三份报告全部存在、清单完整、源码/二进制/run 身份一致，跨组 challenge 和 VM 也不得复用。原有 **120 秒** 单项上限保持不变。当前 CTest 注册为 **569 + 570 + 801 = 1940**；其中每平台/后端的 306 个内部场景不另冒充 CTest 数量。

首次未分组运行也完整保留：Windows 普通构建所选 **8/8** 通过；ASan 所选 **6/8**，两个 306 例产品运行各触及 120 秒上限，见 `upgrade54j-native-asan.*`。随后只改测试分组和聚合，产品源码未变，再进行上述实际 ALL/打包、224 回归和完整 232 门禁。开发期首次 350 项参考捕获中另有 8 个 held readAtLeast 等待条件写错，修正后再采干净的 306 项基线；最初脚本与失败数据 `upgrade54c/d-*` 未被覆盖。早期总控制中的独立 JUnit 固定清单遗漏在 `upgrade54g-*` 检出，补齐后完整复验，记录 `upgrade54h/l-*`。上述失败均未改写为通过。

**还有 44 个 JS 字节流组合未纳入本轮正式通过。**每平台开发期观测包括：8 项 minimum 超容量的正常参数校验、12 项非对齐 EOF 内部错误、6 项 Float64Array EOF 零长度视图但 backing 含五字节、10 项受控的高 minimum 挂起取消，以及 8 项上述脚本等待错误。容量校验属于待补覆盖；其余也须分别处理或确认固定 profile 的取舍，不能统称参考缺陷。清单在 `fetchBYOBDeferredStandardCases`，原始错误引用 ID 保留在捕获中；未靠归一化或放宽期望把它们算成产品通过，详见 `upgrade54q-open-coverage-review.json`。

当前冻结输入 `upgrade54m-project-inputs.json` 与 `upgrade54p-tested-project-inputs` 各 **4518** 份；实际产品、库、对象及选定诊断文件 **418** 份在 `upgrade54p-current-build-binaries`。未分组输入另保存在 `upgrade54m-before-shards`；更早的 `upgrade54a-before-project-inputs.json` 和第 99 轮封存不变。执行记录 `upgrade54m/n-*`，封存 `upgrade54-final-audit.json`；这八份文档在验证完成后单列更新，不混入先前冻结输入。

**本轮没有整体关闭 body 强兼容或 0.3.0。**原生读取的未预读首读、空 EOF、参数与访问器顺序、非法读取器原型借用、释放/重新获取、并发 BYOB、owner/abort/context end、跨 credit window 大 body 与限制边界仍须增补；普通 fetch、tee/clone/values/pipe 和分支资源图仍开放。当前六字节 fixture 不能证明大 body 的 credit 补充，不能用前述 ACK 源码修正代替实测。

版本仍 **0.2.2**、补丁锁 **59**，没有 Root 提交、推送、版本提升或发布。本轮未执行不筛选的 1940 项完整矩阵；历史 **1877/1819/34/24** 与 **1762/1733/6/23** 按各自输入保留，仍有 **15 个旧失败组合未关闭**。默认 Debug 1 MiB Hono、worker 重选、旧 WPT/profile、新输入的独立干净 Debug/Release/Linux LTO、真实 CI、完整依赖 sanitizer/fuzz/soak、正式三车道五轮性能、真实 2 GiB 密度和四视口仍待验收。正式性能发布点仍为 0；T15/T16/T17 未整体关闭。

下一步见 [第 100 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-100)。以下第 99 轮及更早章节按历史输入保留。

<!-- capsid-review-phase-99 -->
<a id="capsid-review-phase-99"></a>

## 第 99 轮（2026-10-20）：原生 body 的直接 DefaultReader 构造路径

本轮修复 `js/websocket.js` 中已确认的读取器关联缺口。原实现只在 `body.getReader()` 关联原生 body 的上下文与挂起读取；现在直接构造也完成相同关联，并保持原构造器的实例校验、`newTarget` 和原型关系。全局构造、`prototype.constructor`、子类和 `Reflect.construct` 均进入实际验证。标准 JS 流继续使用其原有排队读取行为。

正式 fetch transition 由 **30 项增加到 46 项**，保留原 30 项并新增四种构造方式各三项原生行为和一项标准流控制。原生项检查对象取消、首次读取前取消、第二次挂起读取；标准流控制实际排队两个读取后取消，要求两者正常完成。新构造项还核对实例、原型、构造器与原型构造器身份。字节、原因类型、Promise 反应顺序、真实 TCP 收尾、相同 VM、输入和二进制身份等原校验继续生效。

**已记录真实失败到通过的证据。**Windows、Linux 的锁定 workerd 各执行 46 项并通过；修复前实际产品两平台均为 **38 项通过、8 项校验失败**，原 30 项和全部标准流控制通过，失败恰为四种构造方式的取消与第二个挂起读取。原始完整捕获和失败退出保存在 `upgrade53b-*`，逐项审阅在 `upgrade53d-baseline-case-review.json`。参考样本由真实 Linux 46 项执行生成，校验器及拒绝错误/缺项/同错矩阵等独立控制 **1596/1596**；其合成平台/后端复制只用于检验校验器。

| 本轮实际当前主构建 | ALL / 打包 | ABI、策略、上下文、生命周期与包回归 | 完整必需门禁 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 74/74 | 61/61 |
| Windows ASan Debug | 通过 | 74/74 | 61/61 |
| Linux UID 1000，构建类型为空 | 通过 | 76/76 | 77/77 |

本轮回归合计 **224/224**，完整门禁合计 **199/199**，均为本轮修改后实际重新执行，零失败、零跳过。各参考和实际后端的正式报告均包含 46 项；普通 Windows 两后端、Linux 三后端以及 ASan 两后端实际进入同一严格校验。当前 CTest 注册仍为 **1907** 项，内部场景增补不冒充新增 CTest 项。

首次完整门禁实际为 **196/199**，三个失败均为 `workerd_ws_gate_controls`：正式场景已增加到 46，但总控制中一条独立字面期望遗漏更新、仍为 30，内部控制各为 463/464。原始 `upgrade53f-*` 结果保持失败。随后仅将 `tests/test_workerd_reference_gate.mjs` 中该期望改为 46，保留固定清单与全部严格校验，再对同一实际产物完整复跑三套门禁，得到上述最新 **199/199**，记录为 `upgrade53r-*`。这次失败不是产品差异或超时。

ALL/打包和 224 项回归采用 `upgrade53e-project-inputs.json`；最终门禁采用 `upgrade53p-project-inputs.json`，两者只差上述控制测试，产品输入及所有 418 份受保护产物核对未变。两个输入快照分别在 `upgrade53h-tested-project-inputs` 和 `upgrade53q-tested-project-inputs`，各 **4510** 份；产物在 `upgrade53h-current-build-binaries`。执行记录 `upgrade53e/f/r-*`，封存 `upgrade53-final-audit.json`；本段文档在验证后更新，与冻结测试输入单列。上一轮 `upgrade52-final-audit.json` 及先前真实失败、完整矩阵继续保留。

**本轮只关闭上述直接 DefaultReader 路径的已测差异。**BYOB、多字节视图、tee/clone/内部 reader、普通 fetch 与限制包装及 owner/credit/资源图仍待实现和正式成对验收。第 97/98 轮独立干净 Linux Debug、Windows 非 LTO Release、Linux Release/LTO 属于各自旧冻结输入，本轮 JS 变化尚未在这些独立快照重新资格化；不能将旧 Release/LTO 结论扩展为本轮产物资格。

版本仍 **0.2.2**、补丁锁 **59**。本轮未执行不筛选完整矩阵；最近完整 **1877/1819/34/24** 及早期 **1762/1733/6/23** 按原输入保留，按当前所选执行映射仍有 **15 个旧失败组合未关闭**。默认 Windows Debug 1 MiB Hono、worker 重选不稳定、旧 WPT profile、真实 CI/完整依赖 sanitizer/fuzz/soak及正式性能、真实 2 GiB 密度和四视口仍未验收。正式性能发布点为 0；T15/T16/T17 未整体关闭。没有 Root 提交、推送、版本提升或发布。

下一步见 [第 99 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-99)。以下第 98 轮及更早章节保留为历史。

<!-- capsid-review-phase-98 -->
<a id="capsid-review-phase-98"></a>

## 第 98 轮（2026-10-20）：优化构建中的真实分配探针与完整 spawn 异常遍历

本轮 Root 仅修改 `tests/test_abi_guard.cc`。原分配器替换函数在 GCC `-O3 -Werror` 下被内联后，会因 malloc/free 实现及数组分配委托被诊断为不同分配家族；最终为四个分配入口和八个释放入口保留不可内联的探针边界。告警、优化和正常分配/释放配对保持原样，先前仅约束释放入口仍编译失败的中间候选已保留。六种实际 SDK 下的私有验证均完成九模式、无参数及未知模式拒绝，随后才接入 Root。

正式九模式现在对成功 spawn 测得的每个分配点分别注入 `bad_alloc` 和普通 C++ 异常；普通异常必须实际发生，按实际观察到的 nothrow 吞异常情况要求精确的 `OUT_OF_MEMORY / INTERNAL_ERROR`、空输出指针、非空错误及资源守恒。其必要性有真实敏感性反例：只移除构造期间普通异常清理的私有运行库仍通过原覆盖，扩展覆盖检出句柄泄漏。该修正不改变任何产品源码、配额或协议。

| 实际当前主构建 | ALL / 打包 | 本轮 ABI、协议、策略、注册与文档检查 | 既有门禁证据 |
| --- | --- | --- | --- |
| Windows 普通 Debug | 通过 | 23/23 | 沿用第 97 轮完整 61/61 |
| Windows ASan Debug | 通过 | 23/23 | 沿用第 97 轮完整 61/61 |
| Linux UID 1000，构建类型为空 | 通过 | 23/23 | 沿用第 97 轮完整 77/77 |

本轮所选合计 **69/69**，零失败、零跳过。逐项比较了上一轮归档的非探针产物，只有 ABI 分配模型相关文件发生变化；门禁使用的 SUT/辅助二进制和代码、命令及配置均未变，才保留 **199 项完整门禁**实测证据。本轮没有重跑这 199 项，也没有把更换过的测试模型说成相同二进制。第 97 轮主回归 **224/224** 和首轮普通 Windows 门禁 **60/61** 超时及随后完整静置复验均继续保留。

**新干净 Linux Release/LTO 候选已完成实际 ALL、打包、28/28 所选回归和 77/77 完整既有门禁。**实际 SDK 编译和 worker 链接命令均核对到 LTO 标志，源树保持干净；其冻结输入为 `upgrade51al-project-inputs.json`，本轮验证为 `upgrade51am-*`。它与旧失败候选仅有这一份测试文件差异，全部产品输入相同；原 `upgrade51x-*` 的失败 ALL 仍按原输入保留，未被改写。干净 Windows 非 LTO Release 的 **32/32 + 61/61**、干净 Linux Debug 的 **28/28 + 77/77** 是第 97 轮独立实测；Windows 的 LTO 强制关闭仍是既有基线边界。

当前 Root 冻结输入 **4510** 份在 `upgrade52e-tested-project-inputs`，产品/库/对象与选定诊断文件 **418** 份在 `upgrade52e-current-build-binaries`。当前注册仍 **1907** 项，版本 **0.2.2**、补丁锁 **59**；本轮未执行不筛选完整矩阵。最近完整结果仍为 **1877/1819/34/24**，按已执行旧失败项映射仍有 **15 个旧组合未关闭**；早期 **1762/1733/6/23** 单列保留。执行记录 `upgrade52c/d-*`，上一轮封存 `upgrade51-final-audit.json`，本轮封存 `upgrade52-final-audit.json`。本段文档在验证后更新，与冻结测试输入单列。

仍不能将 spawn 的两类 C++ 异常遍历扩展成全部内存/ABI 资格：请求等入口的非首点与事务回滚、绑定及原生 C/OS 分配、资源压力还需扩展。原生 body 直接读取器、BYOB、tee/clone、普通 fetch 与上下文/credit/资源图仍有已知差异，下一轮方案为 `upgrade51aq-native-body-integration-plan.json`。默认 Windows Debug 1 MiB Hono、worker 重选不稳定、旧 WPT profile、当前完整矩阵、平台/CI、完整依赖 sanitizer/fuzz/soak及正式三车道五轮 cold/QPS、真实 2 GiB 密度和四视口仍待验收。正式性能发布点仍为 0；T15/T16/T17 未整体关闭。没有 Root 提交、推送或发布。

下一步见 [第 98 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-98)。以下第 97 轮及更早章节保留为历史。

<!-- capsid-review-phase-97 -->
<a id="capsid-review-phase-97"></a>

## 第 97 轮（2026-10-20）：自定义配置、v4 初始化与真实资源回收

已接入五份产品源码修正和四份测试/注册修正：策略编译中的临时字符串/容器使用可抛出的空构造，快照预留容量并原位构造环境条目；内部结果类型使用可抛出的复制，避免 Debug STL 的隐式 `noexcept` 移动在分配失败时终止。worker 构造和 v4 初始化的普通异常也会回收 IPC 与子进程。协议、额度和策略决策规则保持既有定义。

正式 spawn 分配测试已覆盖九种有效配置：默认、显式空/非空 egress、空能力策略、权限规则、能力快照、v4 无通道/完整通道、快照加完整通道。每条路径先测成功分配数，再逐点注入并验证首个越界点成功；`nothrow` 也实际参与注入。Linux 新增逐点描述符守恒及子进程检查，并修正了“回收遗留僵尸后再宣称通过”的检查盲区；真实泄漏描述符和遗留子进程两个私有反例均被拒绝。独立注册契约逐项约束名称、实际模式参数、60 秒上界及正常退出，保留已有 TSan 排除边界。

| 实际构建 | 所选真实回归 | 完整既有 workerd required gate |
| --- | --- | --- |
| Windows 普通 Debug 主构建 | 74/74 | 61/61，静置复验 |
| Windows ASan Debug 主构建 | 74/74 | 61/61 |
| Linux UID 1000 主构建，构建类型为空 | 76/76 | 77/77 |
| 独立干净 Linux 显式 Debug | 28/28 | 77/77 |
| 独立干净 Windows Release，LTO 关闭 | 32/32 | 61/61 |

前三套当前主构建实际完成 `ALL` 和打包，所选回归 **224/224**，完整既有门禁的最新完整执行 **199/199**；后两套独立干净候选分别完成全新 `ALL`、打包、身份及包消费者/复现检查，不能替代全量矩阵。Windows Release 的四项 Hono 测试均以 **1048576 字节** JS 栈通过；普通 Windows Debug 的四项旧失败仍没有被关闭。

普通 Windows 的首次门禁实际为 **60/61**：`workerd_ws_gate_controls` 在既有 **120 秒**上界超时，原汇总因失败只列出零条结果，原始 JUnit 记录全部 61 条。该失败和收集差异保留于 `upgrade51e-native-*`、`upgrade51y-*`；静置后使用相同源码、二进制和上界执行完整门禁，结果为 `upgrade51z-native-*`。没有将补跑单项拼接成一次完整通过，也没有改动原失败。

当前注册 **1907** 项，本轮未重跑不筛选全量。最近完整矩阵仍是第 94 轮 **1877/1819/34/24**（选择/通过/失败/跳过），三配置均退出 8；更早的 **1762/1733/6/23** 单列为历史。按第 95/96 轮所选复验映射，旧失败组合累计关闭 19 个，**15 个旧组合仍未关闭**。本轮新增配置曾在两套 Windows 各有七种实际失败，现已纳入正式用例验证；不得据此改写旧全量统计。

**仍待接入的测试修正与新失败：**独立 Linux Release/LTO 的首次 `ALL` 在旧分配器探针的 `-O3 -Werror=mismatched-new-delete` 编译处失败，记录 `upgrade51x-*` 保持失败。下一版探针为分配/释放入口保留不可内联边界，并对成功 spawn 测得的每个分配点追加普通 C++ 异常，按实际观测的 nothrow 吞异常情况要求精确返回值；六种实际 SDK 配置共 **54/54** 模式、六次无参数运行及六次未知模式拒绝均通过。仅移除普通异常构造清理的私有运行库能通过原测试，却被扩展测试检出句柄泄漏，证明新增覆盖有效。阶段性候选失败、最终候选和反例见 `upgrade51af/ah/ai/v-*`；待应用文件为 `upgrade52a-staged-abi-guard.cc`。这些私有通过尚未关闭原始失败的完整 `ALL`。另一独立干净候选 `upgrade51al/am-*` 正按新输入验证，结果单列。

Windows LTO 会被现有基线配置强制关闭，本轮请求启用后的配置断言正确识别了实际值；`upgrade51u-*` 保留该结果。Windows Release 的资格范围明确为非 LTO，Linux 承担实际 LTO 验证；没有把请求值当成实际构建选项。

冻结测试输入 **4510** 份，版本仍 **0.2.2**、补丁锁 **59**，在 `upgrade51j-tested-project-inputs`。三主构建的产品、库、对象及选定诊断文件 **418** 份在 `upgrade51j-current-build-binaries`；独立构建的选定输出另存 `upgrade51ag-*`，其中首次 LTO 失败缺少的输出已明确列出。构建/回归见 `upgrade51d/e-*`，静置门禁见 `upgrade51z-*`，正式统计见 `upgrade51an-main-spawn-statistics.json`，本轮封存见 `upgrade51-final-audit.json`。本段是验证后文档，不混入冻结输入。

强兼容仍未整体验收：原生 body 的直接读取器、BYOB、tee/clone、普通 fetch、上下文/credit/资源图，默认 Debug Hono、worker 重选不稳定、旧 WPT 默认 profile、全量复验、其他平台/真实 CI、完整依赖 sanitizer/fuzz/soak及正式三车道五轮 cold/QPS、真实 2 GiB 密度和四视口仍待处理。当前探针主要完整遍历 spawn 的 C++ 分配；请求等入口仍只有代表性首点，C/OS/绑定压力不等于已覆盖。正式性能发布点仍为 0，T15/T16/T17 未整体关闭。没有 Root 提交、推送或发布。

后续执行顺序见 [第 97 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-97)。以下第 96 轮及更早章节保留为历史。

<!-- capsid-review-phase-96 -->
<a id="capsid-review-phase-96"></a>

## 第 96 轮（2026-10-20）：默认 ABI 分配失败、资源回收和测试覆盖修正

三套当前主构建均完成实际 `ALL` 和打包。默认 spawn 的 MSVC Debug 分配失败原先会在 STL 空容器的 `noexcept` 构造中终止；现改用可抛出的空构造形式，并用字段交换避免 Debug 移动构造中的额外分配。Windows 命令行构造期间发生异常时，两个 IPC 描述符由局部守卫回收。原有 172 项检查可通过但累计留下 12 个句柄的反例已保留；修复后的同一原始检查实测句柄差为 0。

| 实际配置 | 本轮所选真实回归 | 完整既有 workerd required gate |
| --- | --- | --- |
| Windows 普通 Debug | 66/66 | 61/61 |
| Windows ASan Debug | 66/66 | 61/61 |
| Linux UID 1000，构建类型为空 | 68/68 | 77/77 |

所选回归合计 **200/200**，既有 required gate 合计 **199/199**，均零失败、零跳过。回归包含真实 ABI/协议/策略/生命周期、包消费者和新鲜包复现；新增独立的无 worker 参数用例。默认分配用例先测量成功路径，再遍历每个 C++ 分配点并验证首个越界点成功：Windows 两套各 **58 个失败点 + 成功路径 / 346 项检查**，Linux **13 个失败点 + 成功路径 / 97 项检查**；无 worker 模式分别 **14 / 14 / 28** 项检查。`nothrow` 单值/数组注入有真实反例，Windows 每点检查句柄守恒并核对最终无子进程；异常终止会立即报失败。独立注册审计约束实际命令、参数、60 秒上界和正常退出状态，并保留原 TSan 分配器符号冲突的排除边界；TSan 元数据控制不代表新 TSan 产品资格。

<a id="capsid-review-phase-95"></a>
第 95 轮已合入描述符回归、配置中的构建工具选择、短嵌套目录、显式空消费配置和包复现工具传递，实际所选 **34/34 + 34/34 + 36/36 = 104/104**。首次收集器漏算 CTest 自动包 fixture 的断言、Linux CLI 包装器被错误传给原生工具覆盖变量后的挂起、相应恢复和修正后的完整复验均保留。最终夹具直接调用 CMake 选定的 CLI，并核对工具锁版本与全部八个真实运行时分组。

当前注册 **1883** 项，但本轮没有重跑不筛选全量。最近完整矩阵仍为第 94 轮 **1877 / 1819 / 34 / 24**（选择/通过/失败/跳过，三配置退出码 8）；更早 **1762/1733/6/23** 仍是另一历史结果。两轮所选复验共关闭旧失败组合 19 个，**15 个旧失败组合仍未关闭**：普通 Windows 的 worker 重选两项、默认 1 MiB Hono 四项、两项旧 WPT 与干净源码身份；ASan 和 Linux 各为两项旧 WPT 与身份前提。这是旧失败项映射，不能当作新的全量失败统计。

**新增且当前主库未关闭的边界：**扩展为默认、显式空/非空 egress、空能力策略、权限规则、能力快照、v4 无通道/完整通道和快照加完整通道共九种有效配置后，两套 Windows 当前库均有 **7/9** 配置失败。显式空策略第 2 点异常终止、权限规则的字符串构造、快照中的容器移动，以及 v4 第 41 点留下两个句柄和子进程都有实际失败记录。`upgrade50e/j/k/q-*` 保存失败和中间候选；`upgrade50s-*` 的组合候选两套 Windows 均 **9/9** 通过，`upgrade50t-*` 在 Linux 上 **9/9** 通过，独立无参数模式成功、未知模式失败。Linux 当前库的同九种模式也通过，不能把 Windows Debug STL 问题泛化为 Linux 现有失败。组合候选尚未合入本轮冻结主输入；下一轮须消除候选中发现的重复 reserve，并补正式注册、独立契约、Linux 描述符和僵尸检查后重建。默认路径通过不代表所有自定义策略、绑定、v4 或 OS/C 分配失败均已覆盖。

测试输入 **4510** 份，版本仍 **0.2.2**，补丁锁 **59**。当前 `src/js/include` 已有 C++ 变更，不能沿用第 94 轮“153 份产品输入未变”的历史表述。完整冻结输入在 `upgrade50g-tested-project-inputs`，当前产品/库/对象及选定诊断文件 **418** 份在 `upgrade50g-current-build-binaries`；不是所有文件都是可执行程序。执行记录为 `upgrade50c-*`、`upgrade50d-*`，最终审计为 `upgrade50-final-audit.json`；第 95 轮封存为 `upgrade49-final-audit.json`。本段文档在验证后更新，与冻结测试输入单列。

完整 0.3.0 强兼容仍未验收：原生 body 的直接读取器、BYOB、tee/clone/普通 fetch 和资源图差异，默认 Hono、剩余旧失败、显式 Linux Debug、干净 Release/LTO/ALL/包及消费者、完整依赖 sanitizer/fuzz/soak、其他平台/真实 CI、正式三车道五轮 cold/QPS、真实 2 GiB 密度和四视口仍待处理。现有 Linux `CMAKE_BUILD_TYPE` 为空；Windows ASan 配置的通过不代表完整依赖插桩。正式性能发布点仍为 0，T15/T16/T17 未整体关闭。没有 Root 提交、推送或发布。

本轮交接与后续顺序见 [第 96 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-96)。以下第 94 轮及更早章节保留为历史。

<!-- capsid-review-phase-94 -->
<a id="capsid-review-phase-94"></a>

## 第 94 轮（2026-10-20）：默认编码流栈修复、完整三配置基线与新增差异

QuickJS 补丁增至 **59**：在解释器调用内复用五个不可变 JSValue 字面值，普通 MSVC Debug 的实际固定帧从 **20504 降到 17992 字节**，原编译选项、Debug 检查和默认 1 MiB 预算保持不变。三配置针对性回归各 **23/23**。同时将 SingleWorker 实现纳入 Boost 启用时的 Host core 静态库，移除单个往返测试的链接补偿；三配置实际 **ALL** 均完成。

| 实际配置 | 完整 workerd required gate | 不筛选完整 CTest：选择 / 通过 / 失败 / 跳过 |
| --- | --- | --- |
| Windows 普通 Debug | 61/61 | 549 / 523 / 16 / 10 |
| Windows ASan Debug | 61/61 | 550 / 528 / 12 / 10 |
| Linux UID 1000，`CMAKE_BUILD_TYPE` 为空 | 77/77 | 778 / 768 / 6 / 4 |

全量合计 **1877 / 1819 / 34 / 24**，退出码均为 8；门禁通过没有关闭全量失败。原始 JUnit、日志和逐项失败/跳过均保留在 `upgrade48l-*`。此前 Linux 的笼统 Debug 名称应以本轮实际缓存为准，不能据此声明显式 Debug 资格。Windows 普通 Hono 仍在 1 MiB 下四项溢出；ASan Hono 显式使用 2 MiB，不能用于关闭默认预算兼容。

原生 body 的直接 DefaultReader、BYOB、tee/clone 与普通 HTTP 仍有实际参考差异；选择集详情见 `upgrade48r-*`、`upgrade48s-*`、`upgrade48v-*`。普通 HTTPS 的 Host 环境变量信任设置不等同于已有 C-ABI 显式 CA 设置，须先校正测试前提。独立描述符修正已验证 Windows 24/24 + ASan 24/24，新模型在旧库实际失败、候选通过 17 项，Linux 严格编译通过 14 项；这些候选在本轮完整基线中尚未合入。配置的 Windows esbuild 可使原八组运行时反例在两配置通过；短目录可使原 ASan 配置/消费反例通过；Linux 空配置的消费候选也已保留正反例。

Root 测试输入 **4509**，版本仍 **0.2.2**；`src/js/include` 的 153 份输入未变，但运行时确实经补丁 59 重编。全部测试输入另存 `upgrade48ax-tested-project-inputs`，当前二进制/库/诊断文件共 **409 份**另存 `upgrade48al-current-build-binaries`。完整强兼容、干净 Release/LTO/包/消费者、剩余失败与平台前提、正式三车道/2 GiB 密度/四视口、完整依赖 sanitizer/压力/soak 均继续待办，T15/T16/T17 未整体关闭。没有 Root 提交、推送或发布。

本轮完整结果、实施边界和接棒顺序见 [第 94 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-94)。证据完整性审计为 `upgrade48-final-audit.json`；具体差异方案为 `upgrade48an-implementation-and-test-review.json`；已验证的下一组修复暂存为 `upgrade48az-next-fixes.patch`。以下第 93 轮及更早是历史记录，旧 **1762/1733/6/23** 也继续保留为独立历史基线。

<!-- capsid-review-phase-93 -->
<a id="capsid-review-phase-93"></a>

## 第 93 轮（2026-10-20）：五栈 QPS 控制器与归档已接通

当前 QPS 公开入口已替换旧 shell，默认五轮/managed/strict，覆盖五栈三 workload、独占端口、PHP 双根与 Go 客户端拥有权、先 trickle 后另起 loaded、精确就绪/响应、递归稳定 PSS、清理和封存自校验。正式模式要求干净构建/会话元数据；短 `--observation` 无发布 envelope。下方旧的“QPS 生产器仍未接通”属于历史状态。

最终工具选择集 Windows 普通/ASan **14/14、14/14**（Python 101：70 通过/31 平台跳过），Linux **18/18**（120/120）；没有本轮完整产品重编或 sanitizer 资格。真实五栈 15 单元短观测、2,191 份封存文件与测量中 SIGINT/SIGTERM **10/10** 通过；共享 cold/density 实际小观测也通过。旧验证器 10 个失败及首次 Linux PSS 不稳定失败均保留，最终 Linux 单独复核/顺序回归未放宽协议。

完整 0.3.0 强兼容、T15/T16/T17 仍未整体关闭。干净 Release/LTO/ALL/包与消费者、完整 gate/矩阵、同一安静会话正式三车道/2 GiB 密度/四视口、原生 body/压力/soak、Windows 默认 Debug 编码流根因和其他平台/依赖 sanitizer 仍待完成。历史 **1762/1733/6/23** 未重跑；版本仍 0.2.2、4508 输入，运行时/Host 153 输入未变，无 Root 提交/推送/发布。

实现、真实与合成测试边界、保留的失败、证据入口和接棒任务见[本轮完整交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-93)。最终审计：`upgrade47-final-audit.json`；Linux 最终结果用 `upgrade47q-linux.json`，不能将失败的 `upgrade47m-linux.json` 重标为成功。

<!-- capsid-review-phase-92 -->
<a id="capsid-review-phase-92"></a>

## 第 92 轮状态（2026-10-20）：QPS 负载正确性/原始证据修复，完整生产器继续

修复真实 Go 负载工具的流正文抽查漏检、错误/超时分类与错误成功 verdict、失败退出码和无效参数；新增完整正文身份及 attempted/checked/completed 计数，正式校验要求 `exact-bytes-v2` 与 30000 ms 截止时间。旧实际工具三组红例和旧验证器 28 个失败断言均保留。最终 CTest 为 Windows **13/13**、Windows ASan **13/13**、Linux **16/16**；新增规则 8 组、Linux 实际 HTTP 8 组、合同增补 2 组。

一次真实工具的 64/64、5+15 秒轮只面对独占正确性夹具；另跑可信字节码 cold 两次启动/102 响应/38 文件小观测。均不作为正式性能点。`src/js/include` 153 份输入未变，版本仍 0.2.2；旧 QPS shell 仍未接通完整五栈生产器。完整强兼容、干净 ALL/全量矩阵、body/stream 压力、正式三车道/2 GiB/四视口及完整依赖 sanitizer/其他平台仍待完成，历史全量结果不更新。

范围、平台边界、二进制/原始失败与下一步见 [第 92 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-92)。最终审计：`build-v030-dev/evidence/native-asan-qualification-20261018/upgrade46-final-audit.json`。没有 Root 提交、推送或发布。

<!-- capsid-review-phase-91 -->
<a id="capsid-review-phase-91"></a>
## 第 91 轮状态（2026-10-20）：编译器发布兼容修复，完整目标继续

编译器补齐不支持 `RENAME_NOREPLACE` 的 Linux 文件系统后备路径与明确系统错误诊断；新增归属跟踪，避免在创建失败后删除既有临时文件。新增 8 组真实编译器回归与 7 组 C++ 操作模型，最终 CTest 为 Windows **12/12**、Windows ASan **12/12**、Linux **14/14**；挂载盘另跑 **8/8**。Windows 的符号链接组及原有 Linux 专属组继续明确跳过。源/字节码三尺寸实跑 **12 次启动、612 次响应校验、123 份封存文件**，仅作功能观测。

首轮 C++11 编译失败、Linux 链接缺依赖、旧编译器真实红例均保留。`src/js/include` 153 份实现输入未变；版本仍 0.2.2。完整 0.3.0 强兼容、QPS、干净 ALL/全量矩阵、正式三车道/2 GiB 与四视口复核仍未完成。历史 1762 项全量结果不作更新；没有新性能点、Root 提交、推送或发布。

完整范围、平台跳过、原始证据与下一步见 [第 91 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-91)。审计入口：`build-v030-dev/evidence/native-asan-qualification-20261018/upgrade45-final-audit.json`。

<!-- capsid-review-phase-90 -->
<a id="capsid-review-phase-90"></a>
## 第 90 轮状态（2026-10-20）：cold 控制器/归档接通，完整目标继续

冷启动公开入口已统一 C-ABI 源码/字节码、Node/Deno/workerd 的进程与端口拥有权、精确响应、固定成员/稳定递归 PSS、独占夹具/原生临时编译、CPU 绑定恢复、丢弃轮与测量轮归档及真实校验器。旧验证器漏检丢弃/进程/warm 证据的 15 个实际红例已修正，原始失败与修前源码保留。正式模式要求完整协议和实际干净构建/会话元数据；观测模式不能发布。

最终工具 CTest：Windows 普通 8/8、Windows ASan 8/8、Linux 10/10（`upgrade44h-*`）。新增 Python 组 21，Linux 全通过，Windows 新增为 10 通过/11 个 Linux 专属跳过；Windows 全部脚本共 67 组、46 通过/21 跳过。Linux 共 74 Python 组及既有 10 C++ 模型通过。本轮没有产品编译或新增原生 sanitizer/泄漏验收。

最终真实 cold smoke 覆盖五路径/三尺寸、30 次启动和 1530 次响应，318 份文件核验（`upgrade44i-*`）；只有一测量轮，不能代替正式五轮。四类真实 SUT 的 SIGINT/SIGTERM 共 8 次清理通过（`upgrade44j-*`）；密度入口回归 12 探针/4 小池单元、174 文件核验（`upgrade44k-*`）。合成完整档案只在临时仓库测试，未写入 data.js。站点按新 HTTP 协议展示 warm 值，旧点仍是未验证历史观测，视觉四视口复核未完成。

**完整 Linux/Windows 0.3.0 强兼容仍未完成。** QPS 合同、编译器挂载文件系统/错误诊断、实际同会话三车道与 2 GiB 密度、更广 body/资源压力、默认 Windows Debug 栈失败、新 ALL/Release/LTO/包/全量矩阵及平台和动态验证仍需继续。4495 输入、版本 0.2.2、58 补丁，Root HEAD 和 `src/js/include` 153 份实现输入不变；没有提交/推送/发布。

完整实现、证据与后续顺序见 [第 90 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-90)；审计入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade44-final-audit.json`。

<!-- capsid-review-phase-89 -->
<a id="capsid-review-phase-89"></a>
## 第 89 轮状态（2026-10-20）：cold 探针已修，整车道与强兼容目标未完成

冷启动 C-ABI 工具已改用公开 worker PID，严格核对状态/请求 ID/完整正文，warm 失败、超时、进程退出/被回收/PID 替换及不可读/零/不稳定 PSS 都失败。worker 子孙树在首次响应后、warm 前固定；冷启动计时在 warm/PSS 之前结束；中断会通过 C-ABI 销毁回收 worker。旧错误成功行和中断遗留的真实红例已保存（`upgrade43d-*`），独立零 tick 解析红例也已修正并纳入最终模型。

最终实际工具 CTest：Windows 普通 6/6、Windows ASan 6/6、Linux 8/8（`upgrade43k-*`）；新增 10 组跨平台 C++ 模型与 7 组 Linux 真实进程回归。另对 10k/100k/1m 的源码/可信字节码实际做 6 次 strict smoke、306 次响应核验并封存，见 `upgrade43p-real-smoke.json`。这些不是正式 cold lane 或新的 workerd 性能比较。新编译测量/模型目标及 Linux bytecode compiler；既有 host/worker、运行时库和 `src/js/include` 153 份实现输入未变，没有新整产品/泄漏/不筛选矩阵验收。

**完整目标仍在进行。** cold shell 控制层尚需统一拥有权/端口/档案契约，QPS 也未接通；真实同会话三车道和 2 GiB 密度、站点视口、更广 body/资源压力兼容、默认 Windows Debug 栈失败、新 ALL/Release/LTO/包与原计划平台和动态验证仍未完成。4489 输入、版本 0.2.2、58 补丁、Root HEAD 不变；没有提交/推送/发布，也没有生成新性能点。旧矩阵与跳过数保持历史性质。

完整修复、证据和下一步见 [第 89 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-89)；审计入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade43-final-audit.json`。

<!-- capsid-review-phase-88 -->
<a id="capsid-review-phase-88"></a>
## 第 88 轮状态（2026-10-20）：密度控制与档案生产器已接通

密度入口已改为 Python 控制器：managed/strict、端口租约、监听拥有权、PID/启动时间、递归正 PSS 与稳定读数、READY 后固定进程成员、逐 worker 覆盖、受控回收、最终验证和 cap 下界。正式运行要求实际构建/会话元数据，受限观测明确不可发布；夹具/运行器先快照并复核，不覆盖共享 APPS/已有 OUT。启动中断、OUT 竞态和 worker 已回收导致漏算三组真实红转绿证据已保存；修正后工具 CTest/实际入口均已重新执行。

实际工具 CTest 为 Windows 普通 **5/5**、ASan 配置 **5/5**、Linux **6/6**，注册数 **542/543/768**。新增 Python **31 组**：Linux 全过，Windows 21 通过/10 个 Linux 专属组跳过；另重跑原 15 组证据校验。没有新增产品编译、原生 sanitizer 或完整 required gate。最终真实入口完成 12 次探针、4 个小池观测单元、173 个封存文件；实际 SIGINT/SIGTERM 后无残留进程/发布单元。正式全单元档案只用临时合成探针验证，不是性能数据。

**T15 的 QPS/cold、实际 2 GiB 与同会话完整采样、整个 0.3.0 强兼容仍未完成。** 新确认 cold 工具从父侧 socketpair 的 SO_PEERCRED 推断 worker PID，实机机制验证返回创建方而非子进程；公开 `capsid_worker_pid()` 已存在，优先修正并验证 worker 子树、响应内容、warm 失败与稳定 PSS。本轮未改 cold 工具。

Root 4486 输入，版本 0.2.2、补丁 58，运行时 153 个实现输入未变；无提交/推送/发布。最后全量仍是 compat27b **1762：1733 通过、6 失败、23 跳过**，Windows Debug 编码流旧失败继续保留。详细实际范围、来源和下一步见 [第 88 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-88)；入口为 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade42-final-audit.json`。下方旧轮次为历史，不代表剩余任务状态。

<!-- capsid-review-phase-87 -->
<a id="capsid-review-phase-87"></a>
## 第 87 轮状态（2026-10-20）：修正性能采集与网页证据边界

T16 的正式采集入口已分离于宽松解析器：必须有完整封存的 lane manifest、原始样本、正确性和隔离记录，并实际重算 SHA；重复/缺失行、异常数值、跨车道来源/CPU/内核/二进制冲突、摘要不符、晚到写入均拒绝，density 的身份和证据不再忽略。站点暂存会重放并归档已验证点，旧点标为未验证，cap 明确为下界，内存描述限定采样窗口。

最终工具 CTest 为 Windows 普通 **2/2**、Windows ASan 配置 **2/2**、Linux **3/3**，每套运行 **15 个 Python 测试组**；注册数 **539/540/765**。这不是新产品编译或新增原生 sanitizer 验收。实际旧组合按预期拒绝，43 份历史文件/data.js 保持原样，没有把合成契约夹具当作性能数据。浏览器不可用，四档视觉验收仍待完成。

**T15 生产器接入、真实性能测量、完整 Linux/Windows 0.3.0 强兼容和发布验收继续进行中。** Root 4478 输入，包 0.2.2、补丁 58，无提交/推送/发布；本轮未新跑 ALL、完整 required gate 或不筛选矩阵。原运行时和 Node 门禁沿用已校验的输入连续性，最后全量仍为 compat27b **1762：1733 通过、6 失败、23 跳过**。Windows Debug 编码流旧失败继续保留。

边界、测试证据和后续执行顺序见 [第 87 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-87)。入口为 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade41-final-audit.json`，档案契约见 `bench/perf-evidence.md`。下方旧轮次为历史；不得把 T16 入口通过当成整个 T15/T16 或版本完成。

<!-- capsid-review-phase-86 -->
<a id="capsid-review-phase-86"></a>
## 第 86 轮状态（2026-10-20）：完成 Windows 流栈压力的受控构建对照

Windows 当前及第 84 轮 Debug worker 在 1 MiB JS 栈下均复现编码流的 5 个 RangeError，显式 2/4 MiB 后六子项全过；Linux 在全部测试限制下均通过。当前相同产品源码的独立干净 **Release** 快照在默认/显式 **1 MiB** 下通过六子项，并完成所选流/WPT **17/17**。同驱动、同 bundle、同 JS 配额的对照中，两份可执行文件的 PE 原生栈预留均为 8 MiB。这支持构建配置相关的栈压力判断；具体 Debug 栈帧原因仍未确定，默认 Debug 失败继续按失败保留，产品默认配额未改动。

固定 Windows/Linux workerd 的测试体各 6/6；其模块全局禁止计时器，因此执行上下文适配、原始 bundle 和适配模块 SHA 均明确记录，没有删改子项。Release 守卫要求的干净快照包含第 85 轮 **4474** 份相同项目输入，本地快照提交 `a0f873026414e52ee5eb79f5bd33e948a382787d`；Root HEAD 未变，历史冻结产物保留。构建仅覆盖 worker/integration 及所选流测试，LTO 关闭，不能当成 ALL、完整 required gate、全量矩阵或发布验收。

第 85 轮产品结果继续有效：新增 30 项的真实参考和七组产品组合全过，原生回归 **21/21、21/21、22/22**，实际 required gate **61/61、61/61、77/77**，两端 16 个 Node 模块各 **6173/6173**。本轮为诊断对照，整体 0.3.0 仍在进行中；包版本 0.2.2、补丁 58。最后完整矩阵仍为 compat27b 的 **1762：1733 通过、6 失败、23 跳过**，保持历史分类。

详细方法、适配边界、失败保留和 T15/T16 等后续事项见 [第 86 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-86)。本轮证据入口为 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade40-final-audit.json`。下方旧轮次为历史。

<!-- capsid-review-phase-85 -->
<a id="capsid-review-phase-85"></a>
## 第 85 轮状态（2026-10-20）：新增 30 项转换与生命周期场景已完成专项闭环

**upgrade39** 修正普通 101 后初始 HTTP Promise/定时器过早失活，以及原生 body reader 取消时待决 read、序列化原因、单待决读取与回调顺序的差异。新增固定 **30 项**，两端真实 workerd 和七种产品运行组合均 30/30；包含 TLS IPv6、跨协议重定向、持有 body/响应头/ClientHello 时取消及最后连接结束。原始 ClientHello/HTTP/WebSocket 字节、SNI、证书 SAN、nonce、连接数量、同 VM 与终结检查均有字面断言。

Windows 普通/ASan、Linux 的完整 required gate 为 **61/61、61/61、77/77**，原生定向 CTest 为 **21/21、21/21、22/22**；两端 16 个 Node 模块各 **6173/6173**，新增契约单模块 968 项。注册 **538/539/764**，必需 **133/133/154**。原生退出测试已改为等待并断言正常退出，ag 构建已实际重跑原生测试；完整门禁沿用字节/输入一致的 ac 执行，以连续性证明关联，未重跑整组。

流/WPT 的 Linux 17/17；Windows 普通/ASan 各 16 通过、1 失败（WPT 本体 15/16，另有来源校验）。编码流转换的 5 个 RangeError 在保存的上一轮 worker 中同样复现，仍按失败保留，complete/allPassed 为 false。**整体 0.3.0 强兼容和交付尚未完成。** 包版本 0.2.2、补丁 58、输入 4474；未提交/推送/发布，未新跑冻结 ALL 或不筛选全量。最后全量 compat27b 仍是 **1762：1733 通过、6 失败、23 跳过**，不能用专项结果改写。

证据入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade39-final-audit.json`。详细修正、测试边界、67 个二进制/203 个输入归档和待办见 [第 85 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-85)。下方旧轮次为历史，不再把已关闭的这 30 项当作未实现；BYOB/直接构造读取器/tee 等更广兼容、Windows 流旧失败、完整矩阵和交付门禁仍需继续。

<!-- capsid-review-phase-84 -->
<a id="capsid-review-phase-84"></a>
## 第 84 轮状态（2026-10-20）：新增 fetch 边界已完成专项闭环

**upgrade38** 新增 TLS/SNI、真正的 IPv6、重定向、响应体错误与 Request/Response 转换的固定 **22 项**场景；双端真实 workerd 与七组当前产品各 **22/22**。Windows 普通/ASan、Linux 的完整 required gate 为 **57/57、57/57、72/72**，定向 CTest 为 **21/21、21/21、22/22**，两端 15 个 Node 校验模块各 **5169/5169**。注册 **534/535/759**，必需 **130/130/150**；未用注册数量代替产品执行。

比较器按字面语义检查原始 HTTP/完整 WebSocket 字节、每跳 nonce、证书/SAN、请求数量、前缀后错误、相同 VM 和服务停止前的源端结束。新增边界单模块 640 项，平台单模块 559 项，归档门禁单模块 202 项；组合的 `workerd_ws_gate_controls` CTest 为 456 项。相同错误出现在两端也必须拒绝。Response 的无关 redirect 值与 getter 均不得被观察，初稿的共享初始化回归已经修正。旧产物与初稿的真实反向结果、收集不完整状态和失败验证记录保留。

**整体 0.3.0 强兼容与发布仍未完成。** 包版本 0.2.2、补丁 58、输入 4466，未提交/推送/发布，未新跑冻结 ALL 或不筛选全量矩阵；最后全量仍是 compat27b 的 **1762：1733 通过、6 失败、23 跳过**。证据入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade38-final-audit.json`；详细修正、验证边界与剩余工作见 [第 84 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-84)。下方旧轮次是历史，不应据此重复声明本轮缺口仍未实现。

<!-- capsid-review-phase-83 -->
<a id="capsid-review-phase-83"></a>
## 第 83 轮：代理 22 项、完整必测门禁与测试覆盖边界（2026-10-20）

当前 upgrade37 原生 fetched 101 Response 代理在两端固定 workerd 和七组产品均 **22/22**。Windows 普通/ASan、Linux 定向各 **19/19**，完整 required gate **53/53、53/53、67/67**；注册 **530/531/754**、必需注册 **127/127/146**。两端 14 个 Node 模块各 **4494/4494**，代理/平台/完整归档控制分别 **521/531/195**，无失败或跳过。基础 C/C++ 组件三套各 **6/6**，ASan 审计 **2/2**。

正式 runner/contract 为 `tests/test_websocket_fetch_proxy.mjs`、`tools/workerd-fetch-proxy-contract.mjs`，检查原始握手、完整大消息、双向结束、来源寿命、RequestInfo 和跨请求 API 顺序。旧 Y 原型两端各 **21/22**，唯一失败正是修复前的 accept 错误顺序。实际 Linux gzip 参考仅作为单测向量；合成候选不是产品证据。

Windows fault 夹具以自身 iterator level 0 执行完整 allocation sweep；额外 level 2 Debug 夹具保留普通额度/租约/credit 检查，两项均必需。产品编译选项与 ASan 未放宽。完整归档控制因新增约 19.96 MB 原始数据，实测约 51.19 秒，CTest 限制从 30 秒明确增加到 **120 秒**；完整二进制与所有旧反例保留。

当前 4458 输入、181 个运行相关归档和 57 个二进制见 `upgrade37-final-audit.json`。**未完成新的冻结 ALL/不筛选全量矩阵或 0.3.0 发布**。详见 [本轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-83)；下文第 82 轮及更早为历史。

<!-- capsid-review-phase-82 -->
<a id="capsid-review-phase-82"></a>
## 第 82 轮状态（2026-10-20）：managed 更新保留本地连接来源，修复 Windows 增量缓存

当前候选 **upgrade36** 修复已断开传输、本地发送方向仍开放时，managed 热更新过早回收旧 generation 的问题。新增可选能力 **128**、v4 帧 **28** 和追加 C 事件 **19**，保留原 ABI 结构；未请求该能力的旧协商结果保持原值。Host 以物理通道和保留来源的统一存活值判断 drain，原有 **10 秒**截止时间继续生效。Linux 新增真实 managed 四场景 **4/4**；旧 upgrade35 原产物完整收集 **4 项、仅 2 项符合**，准确检出更新/退休过早回收。此专项验证项目部署生命周期，不宣称 workerd 提供同一部署接口。

Windows 普通/ASan、Linux 当前定向 CTest **17/17、17/17、18/18**；完整 workerd required gate **49/49、49/49、62/62**，无失败/跳过；既有生命周期七组产品各 **19/19**。两端 13 个 Node 模块各 **3932/3932**，生命周期比较器仍为 **686** 项。平台注册总数 **525/526/749**，必需注册 **122/122/141**；注册核对本身不算产品执行。构造器既有 `unrequested-subprotocol` 差异仍单列保留，完整门禁通过不代表所有语义差异已关闭。

Windows 首次真实失败源于旧对象的头文件依赖数为零：仅修正新的编译前缀不足以更新已有缓存。现按编译器/前缀修订一次性移除构建目录内的此类单个对象，重新编译获得依赖；C/C++ 回归保留旧前缀负控制，并验证缓存迁移、后续仅改头文件的重编译和重复配置。两套主缓存各迁移 **368 个**对象；独立新目录各 **6/6**，关键对象分别记录 **35/34** 个依赖；主工程竞态回归普通/ASan 均恢复。失败 JUnit、旧对象/二进制及采集器的 CRLF 解析错误均保留，没有更改旧原始结果。

**整体 0.3.0 强兼容与发布仍未完成。** fetched Response 原生代理耦合、更广资源图/并发压力、新 fetch 的 TLS/IPv6/读取错误边界、三份冻结 ALL 和不筛选全量矩阵继续待办。上一完整候选仍为 **compat27b**，全量 **1762：1733 通过、6 失败、23 跳过**；Root 包版本仍为 **0.2.2**。当前证据入口 `upgrade36-final-audit.json`，详见[第 82 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-82)。以下第 81 轮及更早记录均为历史。

<!-- capsid-review-phase-81 -->
<a id="capsid-review-phase-81"></a>
## 第 81 轮状态（2026-10-20）：延后本地关闭缺口已修复

当前 **upgrade35** 将入站传输 EOF 与本地发送方向关闭分开处理，已修复无子连接时等待事件被过早取消的问题。正式生命周期 **19 项**，双端实际 workerd 与七组产品均 **19/19**；完整 required gate **49/49、49/49、62/62**；两端 13 个 Node 模块各 **3908/3908**，生命周期控制 **686** 项；原生定向 **15/15、15/15、14/14**，ASan 审计 **2/2**。保留原 30 秒控制集限制。

原生回归新增有限保留来源额度、延后关闭、微任务排空、定时器停止、复用与实际正常进程退出。首次三处旧“EOF 即取消定时器”断言失败有双端独立 workerd 探针依据：旧取消夹具现在显式本地 close，原有无副作用断言保留，延后关闭另有严格正向覆盖；失败日志与输入未删除。两端旧产品完整收集 **19 项、语义 18/19**，仅新增的无子连接延后关闭失败。

**整体强兼容仍未完成。** managed generation 留存、更广上下文/资源图、原生 Response 代理、TLS/IPv6、冻结 ALL 与全量验收继续待办。上一全量 **1762：1733 通过、6 失败、23 跳过**仍保留，Root 包版本 **0.2.2**。收尾 `upgrade35-final-audit.json`，详见[第 81 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-81)。下文旧轮次为历史。

<!-- capsid-review-phase-80 -->
<a id="capsid-review-phase-80"></a>
## 第 80 轮状态（2026-10-20）：异常父连接的等待事件与本地 Close

当前 **upgrade34** 修复异常父连接断开时过早终止同源等待事件，以及入站配对端点断开后 close() 未更新本地发送方向状态；构造器/fetched 端点保持独立参照行为。生命周期正式集合 **17 项**，双端实际 workerd 和七组产品均 **17/17**；完整 required gate **49/49、49/49、62/62**；两端 13 个 Node 模块各 **3816/3816**（生命周期比较器 **594** 项）；原生定向 **15/15、15/15、14/14**，ASan 审计 **2/2**。原 30 秒控制集限制及负向用例保留，计数断言的首次失败与精确修正分别归档。

**整体强兼容仍未完成。** 新增且正常清理的延后 close() 探针，两端 workerd 各 **4/4**、当前两端 single-worker 各 **3/4**；没有子连接时的等待事件被过早取消，见 `upgrade35b-gap-audit.json`。它未纳入正式 17 项或门禁通过数，下一步先修此项。HTTP/accept/跨上下文/大资源图、原生代理、TLS/IPv6、冻结 ALL 和全量验收继续待办。上一全量 **1762：1733 通过、6 失败、23 跳过**仍保留，Root 包版本 **0.2.2**。收尾 `upgrade34-final-audit.json`，详见[第 80 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-80)。下文旧轮次为历史。

<!-- capsid-review-phase-79 -->
<a id="capsid-review-phase-79"></a>
## 第 79 轮状态（2026-10-20）：共享 I/O 生命周期与完整必测门禁

当前 **upgrade33** 已按真实 workerd 修正 Upgrade fetch 的共享 I/O 来源生命周期：已 accept 的子 socket 可以维持同源 fetch/定时器，最后一个已提升连接结束时才回收未完成资源。此前按创建连接立即回收的方案 **9/10**，已由本轮两端参考及七组产品分别 **12/12** 的实现替代。新增契约检查原始请求/nonce、帧、Close ACK、EOF、状态续行和静止点；旧失败与输入未覆盖。

实际完整 required gate **49/49（Windows 普通）、49/49（Windows ASan）、62/62（Linux 普通用户）**，均无失败/跳过；当前 13 个 Node 模块两端各 **3653/3653**；定向 CTest **15/15、15/15、14/14**；平台必需注册 **121、121、139**。Windows 控制集原 30 秒超时通过减少夹具重复构造和临时文件开销解决，保留所有负向用例与完整验证，没有延长限制；首次失败和后续实际完整门禁分别保留。既有标准/契约偏差仍明确存在。

**整体强兼容与发布仍未完成。** 异常断开时仍 await 的同源事件、更广 HTTP/accept/跨上下文边界、fetched Response 原生代理、fetch TLS/IPv6/背压和新冻结 ALL/不筛选全量仍待完成。上一完整候选 **compat27b** 的 **1762：1733 通过、6 失败、23 跳过**继续保留；包版本仍为 **0.2.2**。当前入口 `upgrade33-final-audit.json`，详见[第 79 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-79)。下文旧轮次为历史，尤其上一轮的“按创建连接回收”假设已由本轮纠正。

<!-- capsid-review-phase-78 -->
<a id="capsid-review-phase-78"></a>
## 第 78 轮状态（2026-10-20）：公开 Upgrade fetch 基本 13 场景已通过

当前工作候选 **upgrade32** 已接通公开 fetch → Worker → Host 的升级/非 101 body 链路；Windows 普通/ASan 各两模式和 Linux 三模式，**7 组分别 13/13**。普通 fetch 资源各 **32/32**；构造器各 **179 次 / 36 类**，仍保留既有 `unrequested-subprotocol` 差异。主工程定向 CTest **15/15、15/15、14/14**，平台注册 **118/118、118/118、135/135**；实际 12 个 Node 模块两端均 **3206/3206**。新增 C/C++ 头文件修改自动触发重编译的正/负向回归，并在项目 CMake 修复 MSVC 依赖前缀乱码。早期失败和全部原始报告保留。

**仍未完成整体强兼容/发布。** 新 fetch 的父连接回收、直接转发升级响应、该链路 TLS/IPv6/扩展边界和三份冻结 ALL/完整 gate/全量矩阵仍待完成；第一方位 64 现在已协商，不能沿用下文历史的“未宣告/未接通”。上一完整候选为 **compat27b**，原全量 **1762：1733 通过、6 失败、23 跳过**继续保留。Root 包版本为 **0.2.2**。详细实现、精确证据范围与继续步骤见[第 78 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-78)；收尾入口 `upgrade32-final-audit.json`。以下旧轮次为历史。

<!-- capsid-review-phase-77 -->
<a id="capsid-review-phase-77"></a>
## 第 77 轮当前进度（2026-10-20）：Upgrade fetch 基础已验证，公开链路仍待接通

Root 工作候选 **upgrade29a** 已补可选能力 64、HTTP 元数据 codec/Registry、非 101 body 方向和 credit、ABI v7 新增 `capsid_worker_dial_accept_http()`，以及 13 场景正式驱动/比较器。两端实际 workerd **13/13**；旧 compat27b 产品各 **1 通过 / 12 未通过**，不是通过的新产品结果。第一方 Host/Worker 尚不宣告 64，JS fetch / Worker / Host 网络链路、body/取消/代理/TLS 等仍须实施。

主工程三套选定 CTest 均 **6/6**，独立 C11/C++11 严格组件检查也各 6/6；两端 12 组 Node 控制各 **3,206/3,206**。实际注册总数为 Windows **519**、ASan **520**、Linux **742**，平台必需注册核对 **118/118、118/118、135/135**。完整必跑门禁清单已扩为 Windows 45 / Linux 57；注册核对和合成负向控制不能替代产品运行。本轮没有新候选 ALL、完整产品门禁或全部注册测试结果。

完整实现顺序、证据及限制见[第 77 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-77)。证据入口：`upgrade31j-observations.json`、`upgrade31g-protocol-*`、`upgrade31f-units-*`、`upgrade31m-main-*` 与 `upgrade31n-main-*`。早期配置/夹具失败和参照 Close 竞态原始事件均保留；参照的精确例外不适用于产品。上一完整候选仍为 **compat27b**，全量 **1,762：1,733 通过、6 失败、23 跳过**；其余旧待办没有在本轮关闭。Root HEAD / 包版本仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f` / **0.2.2**。**整体强兼容与发布未完成。以下旧轮次为历史。**

<!-- capsid-review-phase-76 -->
<a id="capsid-review-phase-76"></a>
## 第 76 轮当前进度（2026-10-19）：集合内建方法、fetch 请求头与当前全量回归

当前候选 **compat27b**：4,432 个同字节输入、58 个补丁、三套 strict ALL；专项 **70/70、71/71、76/76** 与原生 workerd 门禁 **41/41、41/41、52/52** 均零失败/跳过。正式加密契约 **42 场景**已关闭上一轮两个集合原型差异；fetch **32 场景**核对 Host 非默认端口、精确默认/调用者请求头及实际 accepted socket，原始字节保留重复 header。两端 11 组验证器各 **2,789/2,789**；选定项目 TSan **63/63、0 reports、123 个项目 C++ 输入**，仍非完整依赖插桩。

本轮当前实现的全部已注册测试均已执行：Windows **513（501 通过/2 失败/10 跳过）**、ASan **514（502/2/10）**、Linux **735（730/2/3）**；合计 **1,762：1,733 通过、6 失败、23 跳过**，无测试排除。六个标准 WPT 失败经本次实际参考/产品观察重新审核，与默认 workerd 匹配；原始失败及旧预期均保留。相对前驱新增的 13 项均通过，没有新增失败；这次全量属于 compat27b。整体 0.3.0 强兼容和发布仍未完成。

23 次跳过已逐项分类（13 个唯一测试）：14 次 Linux 专用探针的 Windows 平台跳过、4 次 Windows 本机测试夹具缺口、2 次 perf 前提、2 次 Linux 普通用户权限、1 次 cgroup 控制器前提。同产物额外权限补跑 **2通过/1跳过（缺少cpu.max）**，严格门禁未通过，exit1及原全量skip均保留；详见 `compat27b-current-skip-audit.json`、`compat27b-linux-qualified-root.json` 和 `compat27b-linux-root-prerequisite-review.json`。

另有不在本轮 32 场景内的只读新发现：严格服务端下，Upgrade fetch 的两个参考均返回101和 `response.webSocket`，当前普通/ASan/Linux 的七个实际产品模式均返回400且无 socket，九次都完整结束，见 `upgrade28c-review.json`。较宽松服务端下的原始 deadline 和正常退出另见 `upgrade28b-review.json`。此项尚未修复；先补正式契约并实现该归属 channel/HTTP 元数据路径，再继续 Windows 夹具及其余待办。

完整修复、修复前实际负向证据和剩余顺序见[第 76 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-76)。主要证据为 `compat27b-verification.json`、`compat27b-final-audit.json`、`compat27b-current-full-matrix-audited.json`、`compat27b-current-full-profile-audit.json`；325 份实际输入与 23 份源码差异已归档。Root HEAD / 版本仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f` / 0.2.2，验后只更新八份结果文档。后续须处理 23 项跳过、其余 crypto/fetch/异步边界、D004/D005、完整插桩/干净重建/实际托管 CI，以及 T15/T16/T17。以下旧轮次为历史。

<!-- capsid-review-phase-75 -->
<a id="capsid-review-phase-75"></a>
## 第 75 轮当前进度（2026-10-19）：WebCrypto digest/HMAC 与全量回归闭环

**验后只读补充：** `brand27a-review.json` 已在双端实际 crypto26c 与双端参考复现两个未关闭差异：全局 WeakMap/WeakSet 原型方法替换会影响产品的 key 状态读取与接收者判定。两项不计入本轮 40 场景通过；下一轮先固定捕获相关内建方法、补成正式契约，再对合入后的当前源码进行三套全量。原产品、参考、输入和收尾证据均已保留，实现及正式断言未做验后修改。


当前候选 **crypto26c**：4,430 个同字节输入、56 个补丁，三套 strict ALL 构建通过；专项 **70/70、71/71、76/76**，原生平台门禁 **41/41、41/41、52/52**，均零失败/跳过。正式 digest/HMAC 契约新增至 **40 场景**，双端参考正控制及旧产品完整观察的拒绝控制已保留。11 个模块双端各 **2,704/2,704**；选定项目 TSan **63/63、0 reports、123 个项目 C++ 编译输入**，仍非完整依赖插桩；七个关键原生 C 源码的实际 Windows ASan 命令已核对。

前驱 http25a 最新三套无过滤全量 **1,749 项：1,720 通过、6 失败、23 跳过**，相对前轮消除 15 个失败组合且无新增失败。六个标准 WPT 失败逐项匹配各端实际 workerd 默认行为观察，原始失败和旧预期未被改写。此结果属于 http25a；**当前 crypto26c 的 1,762 项全量尚待实际执行**，整体 0.3.0 强兼容与发布未完成。

结果与后续顺序见[第 75 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-75)。本轮源码差异 22 份、实际输入归档 321 份；核心证据为 `crypto26c-verification.json`、`crypto26c-final-audit.json`、`http25a-current-full-matrix-audited.json`、`http25a-current-full-profile-audit.json`。本地 Linux 启动入口异常的同用户执行记录保留。Root HEAD / 版本仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f` / 0.2.2；本轮验后仅更新八份结果文档。以下旧轮次为历史。

# Testing and Continuous Gates

<!-- capsid-review-phase-74 -->
<a id="capsid-review-phase-74"></a>
## 第 74 轮当前进度（2026-10-19）：超时 abort 桥恢复请求上下文

当前已验收 **http25a**：三套 strict ALL 通过，专项 **67、68、72 项**，必跑门禁 **37/37、37/37、47/47**，选定项目 TSan **59/59、0 reports**（123 个项目 C++ 输入，非完整依赖插桩），十个契约模块两端各 **2,003/2,003**。

修复 deadline abort 桥未进入所属请求，导致自有定时器无法清理的问题。cooperative deadline、Hono/h3/Elysia 和 itty-router 三种生命周期，以及原有续体/请求上下文保护全部通过专项。保留 io24d 三套无过滤的 **1,749 项、1,705 通过、21 失败、23 跳过**原始结果；当前源码未重跑完整矩阵，不把该结果改写为全通过。两项 WPT、WebCrypto、fetch 头、更多资源图、CI 与 T15/T16/T17 仍待完成。

**0.3.0 整体强兼容与发布验收仍未完成**，包版本 0.2.2。完整实现、产物索引及下一步见[第 74 轮记录](v0.3.0-next-ai-handoff.md#capsid-review-phase-74)。以下第 73 轮及更早内容为历史记录。

<!-- capsid-review-phase-73 -->
<a id="capsid-review-phase-73"></a>
## 第 73 轮当前进度（2026-10-19）：fetch 调用者上下文恢复

当前已验收 **io24d**：三套同字节源码的 strict ALL 构建通过，专项 CTest **60、61、65 项**，必跑平台门禁 **37/37、37/37、47/47**。选定项目 TSan **52/52、0 reports**（117 个项目 C++ 输入，非完整依赖插桩），十个契约/门禁模块两端各 **2,003/2,003**。

补丁 0055 修复同步 fetch 后当前请求归属被清空的问题，保留 Binding 资源 owner 的优先级。共享资源场景增加至 **31 项**：hostname/numeric 两条实际路径均验证调用前、返回后、异步回调中的自身发送成功及跨连接发送拒绝；三项原生 owner 控制覆盖 DNS、拒绝路径、正向网络、回调与引用配平。Host 解析补齐保留 `.invalid` 域的异步否定结果；原生必跑控制同时验证无 DNS 发包及普通域名真实 A/AAAA、NXDOMAIN、取消和超时。两端旧产物的 29 项通过、两项新增行为 RED、io24b 的单测计数遗漏、io24c 的 ASan 门禁 DNS 差异与单独编译旧 DNS 行为的负控制均留证。

**0.3.0 整体强兼容与发布验收仍未完成**。下一步优先 WebCrypto 顺序与 fetch Host/默认头，资源/并发扩展、最终全量和 T15/T16/T17 继续待办；包版本仍为 0.2.2。完整实现、验证与交接见[第 73 轮记录](v0.3.0-next-ai-handoff.md#capsid-review-phase-73)。以下第 72 轮及更早内容为历史记录。

<!-- capsid-review-phase-72 -->
<a id="capsid-review-phase-72"></a>
## 第 72 轮当前进度（2026-10-19）：fetch 资源回收与取消语义

当前候选 **resource23g**：三套同字节输入的 strict ALL 构建通过；Windows Release/ASan、Linux Release/LTO 必跑门禁 **37/37、37/37、47/47**，专项 CTest **55、56、60 项**。项目 TSan **47/47、0 reports**（116 个项目 C++ 输入，非完整依赖插桩）；十个契约/门禁模块两端各 **1,923/1,923**。

新增 29 项共享资源场景与原生 DNS/握手/GC/引用控制。修复连接结束后的原生 fetch 回收、取消原因快照、同步 abort 的检查点、空路径 Cookie 缓存阻塞，以及同一主机多端口配置的规则冲突。旧 cross22g 在两端各有 24 项行为 RED 和 2 项配置启动失败，均单独保留。加密操作完成顺序、fetch Host/默认头、更多资源/并发图及最终全量仍待完成，T15/T16/T17 不能整体勾选。

**0.3.0 整体强兼容与发布验收仍未完成**，包版本仍为 0.2.2。完整实现、失败记录、产物索引及下一步见[第 72 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-72)。以下第 71 轮及更早内容为历史记录。

<!-- capsid-review-phase-71 -->
<a id="capsid-review-phase-71"></a>
## 第 71 轮当前进度（2026-10-19）：跨上下文 Promise

当前候选 **cross22g**，三套同字节输入的 strict ALL 构建通过；Windows Release/ASan、Linux Release/LTO 的完整平台门禁分别 **33/33、33/33、42/42**。专项 CTest 分别 **40、41、43 项**；项目 TSan **39/39**（114 个项目 C++ 输入，仍非全部依赖插桩）。

新增 26 项跨请求/连接 Promise 输入与原生引用释放控制。核对创建/跟随/完成及晚挂 catch 的上下文、微任务和后续轮次顺序、真实两连接 I/O、完整帧、同一 realm 与合作退出。九个契约/门禁模块两端各 1,296/1,296；旧 async21c 在相同最终输入上各有 14/26 项实际 RED。

**0.3.0 整体强兼容与发布验收仍未完成**，包版本仍为 0.2.2。完整实现、原始失败、产物索引及后续任务见[第 71 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-71)。以下第 70 轮及更早内容为历史记录。

<!-- capsid-review-phase-70 -->
<a id="capsid-review-phase-70"></a>
## 第 70 轮当前进度（2026-10-19）：异步回调与执行预算

当前候选 **async21c**，三套同字节输入的 strict ALL 构建通过；Windows Release、Windows ASan、Linux Release/LTO 的平台门禁分别 **29/29、29/29、37/37**，专项 CTest 分别 **36、37、39 项**，零失败、零跳过。项目 TSan **31/31**（110 个项目 C++ 输入，仍非完整依赖插桩）。

新增 24 项共享异步回调用例和 5 项项目预算控制；事件完整字段、错误身份、源位置、晚挂 catch 的同步顺序、监听器实际 I/O、原始帧及同一 realm 健康均有独立契约。静态报告只供校验器测试，门禁要求新进程与实际 SHA。两端分别 904/904 项报告与反例控制通过；旧产品在相同最终输入上均实际 RED。

**Linux/Windows 0.3.0 整体验收仍未完成**，版本仍为 0.2.2；历史 handler15b 完整 CTest/root 不能算作本候选通过。完整实现说明、原始失败、产物索引及后续任务见[第 70 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-70)。以下第 69 轮及更早内容是历史记录。

<!-- capsid-review-phase-69 -->
<a id="capsid-review-phase-69"></a>
## 第 69 轮当前进度（2026-10-19）：连接定时器与 Promise 生命周期

当前候选为 **owner19c**，三份冻结源码包含 **4,399 个同字节项目文件**，三套 strict ALL 构建通过。Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，包版本仍为 **0.2.2**。本轮相对上一候选修改 16 个实现、构建锁和测试输入，其中项目 C++ 为 `src/worker_runtime.cc`；另有八份上轮结果文档已纳入冻结输入。补丁栈由 45 增至 **46**，新增 `0046-connection-async-lifetime.patch`。**本轮连接生命周期专项已验证；Linux/Windows 0.3.0 整体强兼容及发布验收仍未完成。**

前轮定时器诊断暴露的主要差异已进入实现和必跑测试：WebSocket 回调完成不再结束连接资源的所有权；开放连接的一次性、周期和 Promise 链定时器可继续执行。正常 Close 在等待对端 ACK 时仍保留定时器，双方关闭或异常终止后取消；另一 HTTP 请求 clearTimeout 不得取消该连接的计时器，同一连接的下一次事件可以取消。关闭后的 Promise 即使由另一请求解析，其回调也不再执行，包含只修改普通 JS 状态、不调用原生接口的回调。

回收发生在当前微任务检查点之后，已排队微任务和可信错误事件的顺序继续保持。错误事件使用独立的内部清理任务，避免应用定时器取消误删 C18 的错误派发。原生回调另持有有效上下文引用，避免回调中自行清理定时器后留下失效上下文；已撤销但仍被应用 Promise 引用的上下文保持惰性，按引用释放，不再占用活动连接注册项。回收链表不在取消路径额外分配节点。执行入口仍使用有限 deadline；极端 CPU/微任务负载和回调抛错的更广专项尚未验收。

初次 owner19b 完整 Windows 门禁保留 **21 通过 / 2 失败**：出站 WebSocket 已 EOF 时，原始 HTTP 请求仍等待关闭回调中的 15ms 定时器；仅按连接回收会误删该定时器。当前 owner19c 同时检查原始 HTTP token 的 generation 和存活状态，保留该请求尚在等待的出站回调上下文，并在响应终结后回收。入站 Upgrade 仍使用连接生命周期。没有移除原有出站断言；失败报告及实际产品保存在 `owner19b-failure-audit.json` 和 `owner19b-outbound-failed-products/`。

审查额外补入 `allowHalfOpen`：收到对端 Close 只结束接收方向时，关闭事件可以等待定时器，随后发送最后数据并完成发送方向。不能把第一个 Close 误作整个连接结束。此项与前 16 项共同组成当前 **17 项共享生命周期用例**，均以两个官方 x64 workerd 资产的实际运行结果建立独立契约，日期 `2026-08-20`、兼容标志 `[]`。

| 当前 owner19c 验证 | 专项 CTest（含四项 vendor 审核） | 完整平台参考门禁 |
| --- | ---: | ---: |
| Windows Release | 30/30 | 23/23 |
| Windows ASan | 31/31 | 23/23 |
| Linux Release/LTO | 32/32 | 29/29 |

必跑参考门禁现在为 Windows **23 项**、Linux **29 项**，包括每个受支持后端的 17 项 owner 输入和独立契约控制。测试校验真实 Upgrade challenge、完整原始帧与 TCP 对端结束、关闭前后计时次数、完整事件/微任务轨迹、同一 realm 的 HTTP 200，以及实际合作退出。报告绑定 fixture、driver、contract、客户端及 Host/Worker SHA；缺项、双端共同篡改、伪造结果、局部 socket dispose、强制进程停止均不能作为通过。两个保存的旧 18c 产品在同一新 fixture/driver/contract 上实际 RED：`live-one-shot` 只收到 queued，随后 `WORKER POISONED: terminal continuation leak`，HTTP 请求失败；保留完整原始报告和诊断。

原有共享异常集 **80 类 / 161 项**、产品单列边界共 **162 项**、每个参考 **3,009 条 API 断言**和 **36 项 fixture 变体**继续通过，原有 120 秒变体门限未放宽。七个契约/报告测试模块 **479/479**。当前同源 GCC Debug 项目 TSan 扩至 **20/20**，零报告、零失败、零跳过，核对 **105 个项目 C++ 编译输入、21 个选定构建目标**；增加真实 owner 生命周期三后端及原生 Worker WebSocket 回归。它仍是项目 C++ 插桩范围，不能称完整依赖 TSan。原有 `worker_terminal_continuation` 的 HTTP cancel、timeout、停放 reaction、分离定时器严格负向控制继续执行。

实际运行产物、bootstrap、编译器、身份、构建文件，以及本轮受影响的准备后 QuickJS/TJS C 源文件、头文件和四个 C 编译单元对象均核验归档；归档数以 `owner19c-verification.json` 为准。Root 相对冻结输入只在验收结束后更新八份结果文档，运行实现及断言没有验后修改。未做根仓库 commit/push/release。

下一步优先验证定时器回调抛错、CPU/微任务执行预算重入，补非定时器异步资源在连接终止时的回收、并发 owner 与跨连接边界。17 项当前用例不能代替这些更广场景。随后仍需原生 Error 别名、Date/Map/Set/RegExp/ArrayBuffer/typed array/Web API 包装对象混合图与 stack/访问器边缘行为；容量满、背压、ACK 待写、交叉关闭、取消/stop 组合；D004 时钟与 D005 HTML；通用 MSVC 自动依赖捕获、完整依赖 instrumentation/fuzz/soak、Windows/Linux hosted 和独立新目录逐字节构建。最后须对稳定最终源码重跑完整普通/ASan、profile 和 qualified root。

最近完整 CTest 和 qualified root 仍为历史 **handler15b**：Windows 485（473 通过 / 2 失败 / 10 跳过）、ASan 486（474/2/10）、Linux 701（696/2/3），合计 1,672 项：1,643 通过、6 失败、23 跳过，原始退出码均为 8。标准 WPT 的 `support_promise_rejection_events_js`、`MessagePort_initial_disabled_any_js` 在三套中保留；workerd profile 分类不能代替标准符合性。历史 root 3/3 不计为当前通过。

证据根目录：`build-v030-dev/evidence/native-asan-qualification-20261018/`。当前入口为 `owner19c-verification.json`、`owner19c-final-audit.json`、`owner19c-gate-controller.json`、三套 `owner19c-*-focused*` / `owner19c-*-reference-gate/`、`tsan-owner19c-result.json`。独立参考和旧产品 RED 为 `owner19b-canonical-{win32,linux}-{reference,old18c-red}.json`。前轮 `handler19b-owner-lifecycle-diagnostics-v2.json` 的 `qualificationPassed=false` 是修复前历史诊断，保持不改。

> 以下第 68 轮及更早内容为历史快照；当前结论以第 69 轮为准。

<!-- capsid-review-phase-68 -->
<a id="capsid-review-phase-68"></a>
## 第 68 轮当前进度（2026-10-19）：原生异常重建与内建字符串转换

当前候选为 **handler18c**，三份冻结源码包含 **4,393 个同字节项目文件**。三套 strict ALL 构建通过，Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，包版本仍为 **0.2.2**。本轮改动 16 个实现、构建锁及测试文件，没有修改项目 C++ 源文件；新增 `0045-event-string-intrinsic.patch` 后，45 项补丁在干净副本上顺序应用，overlay key/manifest、实际 bootstrap、构建身份及运行产物分别核验归档。**本轮专项已验证，Linux/Windows 0.3.0 整体强兼容和发布验收仍未完成。**

前轮剩余四个基本入口 `name-getter-throws`、`dom-exception`、`aggregate-error`、`override-string` 已进入当前回归。DOMException 通过原生 getter 判断真实类型，使用内部名称、消息及 code 重建；同名覆盖属性不替换内部字段，其 getter 副作用仍按已测顺序发生。AggregateError 保留原生类型及 errors/cause 数据，Error 自有非枚举字段重建为可枚举数据属性；先读取全部自有值，再递归克隆，符号 getter 执行而符号键丢弃。普通对象/数组的循环和别名继续保留，直接或间接回到原生 Error 的循环、函数属性等不可克隆输入按参考回退。数字、null、undefined 名称保持原类型。原始 stack 丢弃，重建 stack 的访问器描述符和异常首行纳入新用例。直接抛出的稀疏数组与函数回退也已验证。

名称 getter 连续抛错时，生成新的 24 位 base32 诊断编号及真实日志记录，错误事件、错误消息、stack 和对象图中的编号必须一致，C17/C18 编号必须不同。比较器先严格验证这些字段、getter 次数、可信派发、微任务顺序、真实线路结束和 HTTP 恢复，再仅对编号做比较归一化；原始报告不改写。全局 String 被替换时，Event/WebSocket 使用捕获的内建转换函数。

| 当前 18c 验证 | 专项 CTest（含四项 vendor 审核） | 完整平台参考门禁 |
| --- | ---: | ---: |
| Windows Release | 28/28 | 19/19 |
| Windows ASan | 29/29 | 19/19 |
| Linux Release/LTO | 30/30 | 24/24 |

初次 18a 门禁的变体锚点和导入重定位问题已修正；18b Windows 门禁通过，ASan 变体组保留原始 120 秒超时。当前 18c 继续采用同一 120 秒门限：完整基线及全部常规后端保持 161 项异常回归，每个故障变体使用绑定目标断言的隔离程序，记录并核对目标用例及实际程序 SHA；普通 runner 没有省略用例的环境变量开关。35 个故障变体及精确诊断继续全部要求通过。

当前共享异常集为 **80 类 × C17/C18 + C18 错误监听器再抛错 = 161 项**，两个官方 x64 workerd 资产各通过 **3,009 条 API 断言**。两端旧 17a 产品均在新 `c17/dom-exception` 指定断言处实际失败。产品独立线路测试每后端执行 12 项原线路用例及 **162 项异常用例**，另含单列 C17 错误监听器再抛错资源边界；客户端清理不计作对端 EOF。比较器/报告控制 **251/251**。实际 fixture 故障集 **36 项**（基线 + 35 个必须命中指定诊断的变体），新增 DOM 私有名称、Aggregate 别名、getter 顺序、原始 stack 残留和诊断字段失联五类。专项及门禁零失败、零跳过，产品要求退出 0、无信号、无强制停止；参考停止按平台保留 Windows SIGTERM/Linux 退出 0。

同源 GCC Debug 项目 TSan **15/15**，零报告、零失败、零跳过，核对 **104 个项目 C++ 编译输入、20 个选定构建目标**。这仍是项目 C++ 插桩范围，完整依赖插桩尚未验收。原有 `worker_terminal_continuation` 的 cancel、timeout、停放 Promise reaction 和分离 HTTP 定时器负向控制继续执行；队列上限、回收窗口、超时、Hono 500ms 冷请求预算及功能栈配置保持既定契约。

25ms 定时器问题已补第一组成对探针：每个平台在前一 18b 和官方参考上各跑 8 项，共 **32 项原始诊断**，两端参考状态一致。异常结束连接后，参考不再运行这些定时器且 HTTP 状态保持 200；产品存在定时器继续执行或 poison 后 HTTP 503。正常 Close 未回复 ACK 的 deadline 单独保留，不计为 EOF 通过。原始证据为 `handler19a-owner-diagnostics-v2.json` 及其四份报告。**这项差异尚未修复**；下一步先补有 ACK 的正常关闭、连接仍开放的合法定时器和严格 HTTP 分离定时器负向对照，再按连接/事件资源所有权修正，不能扩大共享回收窗口来掩盖问题。

随后补齐 9 类生命周期输入，在两个参考与当前两平台产品上各执行一次，共 **36 项新诊断**。两个参考的状态一致，独立核对了定时器次数和实际完整帧：开放连接的一次性/周期定时器正常执行，Close 的 ACK 到达前可继续执行，ACK 后和异常终止后停止；HTTP 正常 await 定时器可完成。当前产品对合法的开放连接定时器也会 poison，因此修复范围包含正常连接生命周期，不能只处理异常清理。HTTP 分离定时器的项目严格 poison 负向控制继续保留。证据是 `handler19b-owner-lifecycle-diagnostics-v2.json`，其中 `qualificationPassed=false` 明确表示尚未修正，不能把采集脚本退出 0 当成通过。

后续首先修复上述连接 owner 生命周期；随后仍需完成更广混合对象图（Map/Set、Date、RegExp、ArrayBuffer/typed array、Web API 包装对象）、原生 Error 别名及 stack 读写/访问器边缘行为；容量满、背压、ACK 待写、交叉关闭、取消/stop 组合；D004 时钟、D005 HTML；通用 MSVC 自动依赖捕获、完整依赖 instrumentation/fuzz/soak、Windows/Linux hosted 执行和独立新目录逐字节重建。最后须在稳定的最终源码上重新执行完整普通/ASan、profile 和 qualified root。当前目标是 Linux/Windows，旧文档中的 Mac 项目单独保留，不作为两平台目标的替代验收。

**最近完整 CTest 与 qualified root 仍是历史 handler15b。** Windows 485（473/2/10）、ASan 486（474/2/10）、Linux 701（696/2/3），合计 1,672 项：1,643 通过、6 失败、23 跳过，原始退出码均为 8。两个标准 WPT 失败 `support_promise_rejection_events_js` 与 `MessagePort_initial_disabled_any_js` 在三套中保留，不能以 workerd profile 分类替代标准符合性。15b 的 root 3/3 不能改称当前通过。

证据根目录为 `build-v030-dev/evidence/native-asan-qualification-20261018/`，当前入口：`handler18c-verification.json`、`handler18c-final-audit.json`、`handler18c-gate-controller.json`、三套 `handler18c-*-focused*` 和 `handler18c-*-reference-gate/`、`tsan-handler18c-result.json`。参考原文位于 `handler18a-shared-reference-v2-{native,linux}/reference.json`，旧产品 RED 为 `handler18a-old-product-red-v2-*.json`。Root 相对冻结源码只更新八份结果文档；未做根仓库 commit/push/release。

> 以下第 67 轮及更早内容为历史快照；当前结论以第 68 轮为准。

<!-- capsid-review-phase-67 -->
<a id="capsid-review-phase-67"></a>
## 第 67 轮当前进度（2026-10-19）：普通异常对象克隆与引擎类型判断

当前候选为 **handler17a**。三份冻结源码为 **4,392 个同字节项目文件**，三套 strict ALL 构建通过。本轮修改 **10 个实现/测试文件**，其中 C++ 变更仅为 `src/worker_runtime.cc` 的内部类型判断辅助函数；CMake 和 vendor patch 文件保持相同，生成的 bootstrap、身份头及实际产物单独按哈希归档。Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，包版本仍为 **0.2.2**，未 commit/push/release。**本轮专项、参考门禁及项目 TSan 验收完成；0.3.0 整体强兼容和发布验收未完成。**

先在两个锁定的官方 x64 workerd 资产与旧 16a 产品上执行 **20 类输入 × C17/C18 = 40 项独立探针**，保留事件、对象属性、转换调用、实际线路、HTTP 恢复及退出记录。名称 getter 抛错的上游内部错误 ID 按原文保留，不以宽泛类别替换。探针确认：旧实现调用普通对象的自定义字符串转换、误判伪 Error 原型、丢失对象图，部分输入没有错误事件并等待截止。正式回归使用相同最终 fixture 在两个官方资产上各执行 **99 项共享异常用例、2,136 条 API 断言**；两个旧产品均在指定的 `c17/object` 断言处实际失败。

`capsidChannelValueKind` 直接使用 QuickJS 的 Error、Proxy、Array 与对象类型判断，避免通过用户原型、`Symbol.hasInstance` 或代理陷阱识别类型。普通对象及其中的数组按可枚举字符串键克隆为新对象：符号键忽略，getter 在同步捕获阶段读取一次，循环和共享引用保留，输出属性为普通可写数据属性。数组空洞与显式 undefined 分开保留；字面 `__proto__` 以数据属性定义，非枚举普通属性忽略。不可克隆的自有函数或代理按已测输入回退为新的 Error，不调用用户的 `toString()` / `Symbol.toPrimitive` / ownKeys 陷阱。原有同步错误派发时序、错误监听器 I/O、C17 late 帧和 C18 final Close 继续验证。

| 当前 17a 验证 | 专项 CTest | 完整平台参考门禁 |
| --- | ---: | ---: |
| Windows Release | 24/24 | 19/19 |
| 原生 Windows ASan | 25/25 | 19/19 |
| Linux Release/LTO | 26/26 | 24/24 |

以上当前验证均零失败、零跳过，保留 `worker_terminal_continuation` 的既有 cancel、timeout、停放 Promise reaction 和分离 HTTP 定时器负向控制。共享异常输入从 **77 增至 99**，共 **49 类 × 两种场景 + C18 错误监听器再抛错**。独立线路测试每个产品后端执行 **12 项原线路用例 + 100 项异常用例**；100 项另含单列的 C17 错误监听器再抛错资源边界。参考门禁覆盖 Windows/ASan 两个后端与 Linux 三个后端。本地比较器控制 **213/213** 通过；实际 fixture 故障集为 **31 项**（先通过基线 + 30 个必须命中指定诊断的变体），新增转换误调用、丢失共享引用和保留伪 Error 原型三项，并拒绝两边同时遗漏新用例或同时损坏引用而产生的伪一致。

相同 Linux 源码的 GCC Debug 项目 TSan **15/15**、零报告、零失败、零跳过，检查 **104 个项目 C++ 编译输入**，包括新的类型判断辅助函数。20 个选定构建目标、实际运行产物和关键编译输入已归档。此范围仍是项目 C++ 闭包，**不等于完整 vendor/dependency instrumentation 验收**。

**原先八组异常输入中，本轮关闭四组普通对象用例：** `object`、`object-primitive`、`object-tostring-throws`、`null-prototype`。另外验证 `object-data`、`object-cycle`、`object-getter`、`object-symbol-value`、`object-proxy`、`object-tostringtag`、`object-fake-error`。普通对象、数组及已列回退输入的通过，不代表全部异常结构化克隆。当前普通对象克隆仍不支持包含 native Error、Map/Set、Date、RegExp、ArrayBuffer/typed array 等更广对象图；原生 Error 的 cause/errors、自有属性、非字符串名称、循环回退和堆栈语义仍未完整实现。新图断言明确不验证引擎生成 Error 的 stack 描述符或格式，不能将其列为通过。

后续按以下顺序继续：

1. **异常兼容。** 在当前 17a 与两个官方资产上继续 `name-getter-throws`、`dom-exception`、`aggregate-error`、`override-string` 四组，以及真实 Error 修改原型、cause/errors、自有属性/描述符、堆栈、循环/别名、混合内建对象图与不可克隆字段回退。另补直接抛出数组/函数、Web API 包装对象的类型判断及回退探针；本轮数组验证限于普通对象内部的数据图。现有 40 项广诊断仅对应原 16a/参考，不能直接改称 17a 全部通过。动态内部错误 ID 保留原文及字段关联、getter 次数、可信事件和真正终止要求；不以一个错误类别掩盖丢事件或未结束连接。补异常身份、更广 getter/监听器重入与跨 owner 操作。
2. **生命周期与资源。** 恢复用 25ms 定时器的 terminal-continuation/poison 观察仍未隔离关闭。按 HTTP/WS owner、创建时机、失败类型和传输终止作成对探针，保留严格 HTTP 负向控制；继续容量满、背压、ACK 待写、交叉关闭、跨 shard/owner、取消/stop 组合。保持原有队列上限、回收窗口、超时、Hono 500ms 冷请求预算及既有 2 MiB 功能栈配置。
3. **专项与交付。** D004 时钟变化、D005 HTML、通用 MSVC 自动依赖捕获、完整依赖 instrumentation/fuzz/soak、实际 hosted/native Mac、独立新目录逐字节重建及最终平台交付仍待完成。

**最近的完整 CTest 与 qualified root 仍属于 handler15b。** 原始全量为 Windows 485（473/2/10）、ASan 486（474/2/10）、Linux 701（696/2/3），合计 **1,672 项：1,643 通过、6 失败、23 跳过**，三个退出码均为 8；两个标准 WPT 失败 `support_promise_rejection_events_js` 与 `MessagePort_initial_disabled_any_js` 未改为通过。15b 的 root 3/3 仍是历史记录。17a 本轮未重跑完整普通/ASan/root/profile，不把旧结果重命名为当前通过；最终实现稳定后须重跑。参考停止结果按平台原样保留（Windows SIGTERM、Linux 退出 0，均非 forced）；产品 Host 要求退出 0、无信号、无强制停止。C17 错误监听器再抛错继续是独立资源边界，客户端清理不计为对端 EOF。

证据根目录为 `build-v030-dev/evidence/native-asan-qualification-20261018/`。当前入口为 `handler17a-verification.json`、`handler17a-final-audit.json`、`handler17a-gate-controller.json`、三套 `handler17a-*-focused*` 和 `handler17a-*-reference-gate/`、`tsan-handler17a-result.json`、`handler17a-tsan-inputs.json`。最终参考证据是 **`handler17a-shared-reference-v2-{native,linux}/reference.json`**；最初不含数组/字面 proto 扩展的同为 99 项参考记录单独保留。当前旧产品 RED 是 `handler17a-old-product-red-v2-*.json`，首次输入记录亦保留。最终 Root 相对冻结源码仅更新八份结果文档。

> 以下第 66 轮及更早内容为历史快照；当前实现、验证范围和下一步以第 67 轮为准。

<!-- capsid-review-phase-66 -->
<a id="capsid-review-phase-66"></a>
## 第 66 轮当前进度（2026-10-19）：异常 getter 与错误监听器的 I/O

当前候选为 **handler16a**，三份冻结源码仍为 **4,392 个同字节项目文件**，三套 strict ALL 构建通过。本轮修改 **9 个实现/测试文件**；C++、CMake 和 vendor patch 输入与已验证的 15b 相同。Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，包版本仍为 **0.2.2**，未 commit/push/release。**本轮专项与参考门禁验收完成；0.3.0 整体强兼容和发布验收未完成。**

先在两个锁定的官方 x64 workerd 资产上执行 **11 类 I/O 输入 × C17/C18 = 22 项探针**，Windows/Linux 的事件轨迹一致；旧 15b 产品在每个平台各有 **18 项**事件、帧或传输结束差异。扩展正式 fixture 后，两份官方参考均通过 **77 项共享异常用例、1,828 条 API 断言**。两个旧产品在指定的 `c17/error-send` 断言处实际失败；原始反例、getter Close 等待截止及正常/强制退出字段均保留。

修复三个可观察行为：C17 错误监听器同步发送时，readyState 已是 CLOSED，仍接受并写出数据；监听器返回后的微任务发送仍转换参数但不再写出数据，忽略的 close 不改变发送方向。监听器内的 close 在未调用用户 close 的分支重抛已保存异常，原有 C18 已关闭方向继续阻止发送。send 的参数转换提前到状态检查前，覆盖 Symbol、对象转换和转换抛错。异常捕获 getter 内及失败微任务中接受的 Close 使用已有 final Close 能力，在写出已接受帧后结束传输，无需对端回复。非法码 1006 的准确诊断也纳入断言。

| 当前 16a 验证 | 专项 CTest | 完整平台参考门禁 |
| --- | ---: | ---: |
| Windows Release | 24/24 | 19/19 |
| 原生 Windows ASan | 25/25 | 19/19 |
| Linux Release/LTO | 26/26 | 24/24 |

以上当前验证均零失败、零跳过。专项新增 `worker_terminal_continuation`，保留 cancel、timeout、停放 Promise reaction 和分离 HTTP 定时器的既有负向控制；不延长回收窗口、超时或 Hono 冷请求预算，不提高容量。参考门禁覆盖支持的全部后端：Windows/ASan 各 single-worker、static-pool，Linux 加 managed。独立线路测试每个后端执行 **12 项原线路用例 + 78 项异常用例**；78 项包含 77 项共享输入和单列的 C17 错误监听器再抛错资源边界。本地比较器控制 **179/179** 通过；实际 fixture 故障集为 **28 项**（先通过基线 + 27 个必须命中指定诊断的变体），新增丢同步写入、跳过转换和忽略 getter Close 三项。

参考停止结果继续按实际平台保存：Windows 为 `SIGTERM`、`forced:false`，Linux 为退出 0、`forced:false`；产品 Host 要求退出 0、无信号、无强制停止。C17 错误监听器同步再抛错保持独立资源边界，客户端清理不计为对端 EOF。断言检查精确帧、getter/转换先后、错误状态、真实终止、HTTP 恢复和有界停止；这些用例不证明完整异常对象克隆或全部重入/背压组合。

**完整矩阵、qualified root、项目 TSan 的最近一轮仍属于 handler15b。** 15b 的原始全量为 Windows 485（473/2/10）、ASan 486（474/2/10）、Linux 701（696/2/3），合计 1,672 项：1,643 通过、6 失败、23 跳过；三个退出码均为 8。两项标准 WPT 失败为 `support_promise_rejection_events_js` 与 `MessagePort_initial_disabled_any_js`，并未改为通过。15b 的 root 3/3、项目 TSan 15/15、104 个项目 C++ 编译输入也仅作历史记录。16a 本轮按 JS 行为改动执行专项与完整平台参考门禁，未重新运行全部 CTest、root 或 TSan，不能把旧结果重命名为当前通过。

还差四组工作，暂不能给出有依据的总完成百分比：

1. **异常对象兼容。** 八组既有未验收输入仍为 `name-getter-throws`、`object`、`object-primitive`、`object-tostring-throws`、`null-prototype`、`dom-exception`、`aggregate-error`、`override-string`。在当前产品与两个官方资产上复用原始诊断，继续核验 native brand、cause/errors、stack、自有属性/描述符、循环/共享引用和不可克隆值回退；补异常身份与更广 getter/监听器重入，不能以名称/消息相等宣称对象图完整兼容。
2. **生命周期与资源组合。** 恢复用 25ms 定时器的 terminal-continuation/poison 输入仍未隔离关闭。按 HTTP/WS owner、定时器创建时机、错误类型和传输终止排列做成对探针，记录回调是否执行、HTTP 恢复、配额释放和退出。继续容量满、ACK 待写、交叉关闭、跨 shard/owner、取消/stop 组合，保留严格 HTTP 负向控制。
3. **标准/profile 验证。** 补 D004 时钟变化和 D005 HTML 的可执行验证，分别记录标准失败和既定 workerd profile。MSVC 自动依赖捕获稳定性也未据本轮重链接宣布解决。
4. **最终交付验收。** 实现稳定后对最终源码做完整普通/ASan/root/profile 验证、完整依赖 instrumentation/fuzz/soak、实际 hosted/native Mac、独立新目录逐字节重建及既定平台交付。本轮仍是本地原生验证。

证据根目录为 `build-v030-dev/evidence/native-asan-qualification-20261018/`，当前入口为 `handler16a-verification.json`、`handler16a-final-audit.json`、`handler16a-gate-controller.json`、三套 `handler16a-*-focused*` 与 `handler16a-*-reference-gate/`、两套 `handler16a-shared-reference-*`、`handler16a-preparation.json` 和 `handler16a-runtime-inputs-*.json`。源码与实际运行/编译输入已按哈希归档；最后仅更新本轮八份结果文档。

> 以下第 65 轮及更早内容为历史快照；当前实现、验证范围和下一步以第 66 轮为准。

<!-- capsid-review-phase-65 -->
<a id="capsid-review-phase-65"></a>
## 第 65 轮当前进度（2026-10-19）：异常快照时序、内置函数与严格回归

当前候选为 **handler15b**。三份冻结源码包含 **4,392 个同字节项目文件**，三套 strict ALL 构建通过。本轮相对 14i 的源码/测试变更为 **9 个文件**，C++、CMake 和 vendor patch 输入保持相同字节。Root HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，包版本仍为 **0.2.2**；未 commit/push/release。**本轮证据复核完成，0.3.0 整体强兼容与发布验收仍未完成。**

| 完整 CTest 矩阵 | 注册/执行 | 通过 | 失败 | 条件跳过 | 原始退出码 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Windows Release | 485 | 473 | 2 | 10 | 8 |
| 原生 Windows ASan | 486 | 474 | 2 | 10 | 8 |
| Linux Release/LTO | 701 | 696 | 2 | 3 | 8 |

三套完整矩阵均未选择或排除测试；原始退出码、失败、跳过、JUnit、LastTest 和注册清单保留。标准 WPT 的 `support_promise_rejection_events_js` 与 `MessagePort_initial_disabled_any_js` 仍失败；默认 workerd profile 的分类不改变标准结果。本轮专项为 Windows **23/23**、原生 ASan **24/24**、Linux **25/25**，参考门禁为 **19/19、19/19、24/24**，均零失败、零跳过。相同 Linux 产物的 qualified root **3/3**、项目 TSan **15/15** 通过，TSan 零报告、零跳过，检查 **104 个项目 C++ 编译输入**；这不等于完整依赖 sanitizer 验收。

本轮先在两个锁定的官方 x64 workerd 资产上记录更广输入，再用旧产品保留明确反例。Windows/Linux 旧 14i 产品均在自定义错误名断言处失败；中间候选 15a 虽通过三套构建，但其 getter 在微任务之后、readyState 3 时执行，两个官方参考均在同步捕获阶段、C17 状态 1 / C18 状态 2 时读取。15a 的产物、输入、失败和未完成状态单列保留，未计为当前通过。

`js/websocket.js` 现在在同步捕获时保存错误名与消息，微任务完成后再派发可信错误事件。自定义名称重建到新的 Error 对象，已知名称使用对应内置类型；非字符串或读取失败的 `message` 使用空文本。内部错误定时器使用捕获的原生函数，`Object.hasOwn` 和格式化函数也保存原始引用。已排队发送、C18 已接受 Close、错误事件的 CLOSED 状态和原有传输边界继续接受精确字节验证。

共享错误输入从 **23 扩到 55**，覆盖 **27 类输入 × 两种场景，加 C18 错误监听器同步再抛错**。新增自定义名称、消息 getter/抛错 getter、Symbol/BigInt、内置函数被改写、返回拒绝 Promise 的错误监听器，以及微任务修改原异常的快照用例。两个实际官方参考均执行 **1,520 条 API 断言**；比较器检查 getter 的次数、状态和先后事件，也检查转换函数未被调用、克隆后的类型及精确帧。**164/164** 项本地比较器控制通过；实际 fixture 故障集为 **25 项**，包含先通过的原始基线及 **24 项**必须命中指定诊断的变体。

独立线序测试每个后端保留 **12 项原线序用例 + 56 项错误用例**；Windows/ASan 各两个后端、Linux 三个后端。56 项中有 55 项共享输入和 1 项 C17 专用资源边界，客户端本地销毁不计为对端 EOF。完整验收后仅修正文末日志计数 `24 → 56`，摘要改为从实际观测清单取数，原断言已执行并检查 56 项；同一批产物的 **7 个后端**已重放修正后的独立脚本，原始全量日志及旧脚本仍保留。最终 Root 相对冻结源码只有八份结果文档和这一处输出计数改动，未改实现或断言。

参考进程停止结果按平台如实记录：Windows 为 `SIGTERM`、`forced:false`，Linux 为退出 0、`forced:false`；产品 Host 要求退出 0、无信号、无强制停止。C17 错误监听器同步再抛错在上游 Linux 中仍阻碍正常停止，继续作为独立资源边界，不并入共享参考正常退出通过。原始 EOF/reset 和客户端调度轨迹保留，不声明所有传输调度一致。

**异常兼容尚有明确缺口。** 本轮通过不代表完整 Error 结构化克隆；`cause`、`errors`、自有属性和对象图没有由当前 55 项证明完整支持。已有更广诊断保留八组待核验/修正输入：`name-getter-throws`、`object`、`object-primitive`、`object-tostring-throws`、`null-prototype`、`dom-exception`、`aggregate-error`、`override-string`。普通对象的字符串化与克隆结果必须分别观察；DOMException 的事件消息与类型、AggregateError 的成员、未知错误名的回退均需补完整断言。原诊断中另有包含恢复用 25ms 定时器的 terminal-continuation/poison 观察；当前 fixture 在错误监听器内恢复内置函数，只隔离该输入，**没有关闭这项独立生命周期缺口**。

后续按以下顺序继续，每项先保留两份官方资产和旧产品的实际反例，再修正实现：

1. 从 `handler15a-probe-fixture-v1.mjs` / `v2.mjs` 和两个平台原始报告复用剩余八组输入。新增 Error/DOMException/native brand、非字符串 name/message、属性描述符、cause/errors/循环引用及不可克隆属性的独立检查；补 getter 内的 send/close/重入，以及错误监听器内和事件结束后的 I/O 操作。动态上游内部错误 ID 保留原文，不能用一个宽泛错误类别掩盖丢事件或未结束的连接。继续扩大 Promise/全局内置替换输入。
2. 单列恢复定时器与错误结束/owner 回收的组合，检查 HTTP 恢复、实际配额释放和有界停止。`worker_terminal_continuation` 已要求 cancel、timeout、停放的 Promise reaction 及分离的 HTTP 定时器触发既定退出；必须保留这些负向控制，辨明 WebSocket event 与 HTTP request 的生命周期边界，不可全局取消回收检查或延长窗口。用 channel races、owned I/O、静态池及 Supervisor 夹具扩展容量满、ACK 待写、交叉关闭、owner 退出、取消和 stop 组合；不因失败提高队列上限、超时或 Hono 的 500ms 冷请求预算。
3. 分别补 D004 时钟变化与 D005 HTML 的可执行验证。标准结果与默认 workerd profile 分别记录；同机单次时钟观察、项目 TSan 或单项专项通过不作为整体关闭证据。
4. 最终实现稳定后补完整依赖 instrumentation、fuzz/soak、实际 hosted/native Mac 交付及独立新目录逐字节重建。本轮为本地原生验证，未声明 hosted CI 已执行；发布验收完成前维持 0.2.2。

本轮还保留 Windows 单次链接文件占用导致的构建失败及同源 strict ALL 重试记录。辅助脚本中残留旧源快照名称被一致性检查拒绝，修正后才启动实际 CTest；没有把准备失败改写为通过。两个 Windows 编译器的 CLI 兼容 ID 与生成头一致；自动依赖当前记录均含该头，通用 MSVC 捕获稳定性仍不据此宣称解决。

证据根目录：`build-v030-dev/evidence/native-asan-qualification-20261018/`。当前索引为 `handler15b-verification.json`、`handler15b-final-audit.json`、`handler15b-resumed-workflow.json`、三套 `handler15b-*-full*`、三个 `handler15b-*-reference-gate/`、`tsan-handler15b-result.json`、`profile-audit-handler15b.json`、`handler15b-code-identity.json` 和 `handler15b-report-replays-*.json`。更广原始诊断、15a 时序反例和准备失败均保留。

> 以下第 64 轮及更早内容为历史快照；当前实现、验证范围及未完成项以第 65 轮为准。

<!-- capsid-review-phase-64 -->
<a id="capsid-review-phase-64"></a>
## 第 64 轮当前进度（2026-10-19）：Windows 原生参考与完整矩阵、关闭及错误事件流程

当前候选为 **handler14i**，三份冻结源码包含 **4,392 个同字节项目文件**，三套 strict ALL 构建通过。包版本仍为 **0.2.2**，Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未 commit/push/release。**本轮证据已完成复核，0.3.0 整体强兼容与发布验收仍未完成。**

| 完整 CTest 矩阵 | 注册总数 | 通过 | 失败 | 条件跳过 | 原始退出码 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Windows Release | 485 | 473 | 2 | 10 | 8 |
| 原生 Windows ASan | 486 | 474 | 2 | 10 | 8 |
| Linux Release/LTO | 701 | 696 | 2 | 3 | 8 |

上述完整矩阵没有使用选择或排除参数，全部注册项均纳入，条件跳过单独列出。原始失败、条件跳过、JUnit、LastTest、注册表和退出码均保留。两个标准 WPT 文件的原始失败继续可见：`support_promise_rejection_events_js` 与 `MessagePort_initial_disabled_any_js`。默认 workerd profile 的审计保留三个标准冲突和一个未到达的旧期待，不把标准失败改为通过。

本轮修正 Close kind 3 被旧负例误判、Windows JUnit 绝对路径被旧审计误判的问题；新增 final Close/能力正反向向量，以及证据路径、原始 CTest 退出码、参考安装器和强制门禁的故障控制。本轮实际专项为 Windows **23/23**、原生 Windows ASan **24/24**、Linux **25/25**，均零失败、零跳过；相同产物上的完整参考门禁分别为 **19/19、19/19、24/24**，均零跳过。Windows 使用自己的官方 x64 资产锁，Linux 使用 Linux 锁，版本、兼容日期和 flags 保持既定 profile。Windows 正式门禁已接入安装器与 CI；本轮为实际本地原生验证，**未宣称 hosted CI 已运行**。

已修正三段真实产品行为：C18 接受 Close 后抛错且客户端不回复时，写出已接受帧后结束传输；C17 已排队微任务中的发送继续被接受，Host 在终止确认前排空已接受的 socket 写队列，`late` 帧不再丢失；同步异常在当前微任务完成后派发一次可信 `ErrorEvent`，状态变为 CLOSED，并以新的异常对象保留七类内建异常及四类非 Error 抛出值的可观察字段。关闭后的 `send()` 诊断与锁定参考一致。

原始线路测试每个受支持后端保留 **12 条**原用例，另执行 **23 条**默认错误事件用例与 **1 条**专用资源边界用例。canonical API 基线为实际 **1,072 条断言**；故障变体为 **21 项**，包含先通过的原始基线及 20 个必须命中指定诊断的错误副本。新增控制拒绝错误双方同时遗漏、过早 CLOSED、丢弃 `late` 帧等伪一致。既有前置验证控制为 **812/812**，本次工作流控制 **70/70** 通过；本候选的实际门禁继续验证注册、字节、事件及故障诊断。

资源边界保持明确：错误监听器自身再次抛错时，C17 的传输仍开放。官方 Linux 参考进程在该输入后不能正常退出，强制结束记录保留；它不计入共享参考门禁的正常退出通过。该分支仍在产品独立线路测试中执行，明确记录至少 100ms 的开放观察与客户端清理，且要求 Host 正常退出，**客户端销毁不计为服务端终止**。C18 的对应分支仍以真实对端终止验证。

新增的内部能力位 `CAP_CHANNEL_CLOSE_FINAL`（bit 5，依赖 CONFIRM）与 Close kind 3 将已接受的 Close 和传输终止作为一个操作；普通 Close、重复关闭和关闭后 DATA 的原规则保留。原有 write/close 限额、4 秒观察上限、Hono 冷请求 **500ms** 预算及既有 **2 MiB** 功能栈配置未提高。两个 Windows 编译器的实际兼容 ID 与生成头一致。本候选首次审计时，Release 编译器单元自动依赖为 **0**、ASan 为 **159**；两套显式依赖边均正确，随后通过更新身份头时间戳在真实 Ninja 图中触发 **四个关键单元实际重编译**，并完成 strict ALL 重链接。头内容与冻结源码未改动；自动依赖捕获在本环境仍不稳定，不宣称通用语言/编码问题已根治。详细前后测量见 `handler14i-identity-dependency-audit.json`。

同 Linux 产物的 qualified root **3/3** 通过，原始完整矩阵中的跳过仍保留。当前源码项目 TSan **15/15**、零报告、零跳过，实际检查 **104 个项目 C++ 编译输入**；范围不等于完整 vendor/dependency sanitizer 验收。实际二进制、包、生成 bootstrap、关键对象、框架/参考运行输入及 **37 个关键变更文件**已归档，索引为 `handler14i-verification.json`。

剩余工作按原计划继续：标准/profile 边界与 D004/D005、更广异常对象形态及异步错误监听器、容量/背压/跨 shard/owner/取消/stop 组合、稳定 MSVC 自动依赖捕获、完整依赖 sanitizer/fuzz/soak、既定平台交付、hosted CI 与独立全新逐字节重建。单次时钟观察、专项通过和项目 C++ TSan 均不作整体关闭依据。

后续接棒按以下顺序执行，每项先保留可复现反例，再修改实现并验证：

1. 扩展 `api-compat.mjs` 与 `ws_handler_errors.mjs` 的异常输入：自定义 Error/对象、特殊字符串化、Symbol/BigInt、全局内建被覆写，以及返回拒绝 Promise 的错误监听器。先用两个锁定官方资产记录事件、状态、字节和停止结果，再更新严格比较器及故障控制；上游进程不能正常停止的分支单列资源边界，不计为共享门禁正常退出通过。
2. 用 `test_channel_races.cc`、`test_channel_io.cc`、静态池故障及 Supervisor 所有权夹具补容量满、ACK 待写、交叉关闭、owner 退出、取消与 stop 的组合。校验实际配额/持有释放和有界停止，并同时保留新能力、旧能力及未知位拒绝的边界；已有预算和队列上限不因测试失败而提高。
3. 分别对 D004 时钟与 D005 HTML 的既定目标补可执行验证。完整标准结果与默认 workerd profile 继续分开记录；实际环境观察不替代时钟变化反例，profile 分类不消除标准失败。
4. 最终实现稳定后重跑全部普通/ASan 矩阵、同源项目 TSan/root/reference/profile，补完整依赖 instrumentation、fuzz/soak、实际 hosted/native Mac 交付与独立新目录重建。未运行项明确保留，发布验收完成前维持 0.2.2。

证据根目录：`build-v030-dev/evidence/native-asan-qualification-20261018/`。当前索引为 `handler14i-verification.json`、`handler14i-*-full*`、三个 `handler14i-*-reference-gate/`、`handler14i-linux-final-reference-gate/`、`tsan-handler14i-result.json`、`profile-audit-handler14i.json` 与 `handler14i-code-identity.json`。14d 取消的部分全量、14e 报告字段误用、14f 后续错误事件反例及 14g 丢失 `late` 帧及 14h 全量发现旧向量/工作流审计遗漏的失败记录均保留，不改名为当前通过。

> 以下第 63 轮及更早内容为历史快照；当前实现、完整测试结果及未完成项以第 64 轮为准。

<!-- capsid-review-phase-63 -->
<a id="capsid-review-phase-63"></a>
## 第 63 轮当前进度（2026-10-18）：SDK 正式修正与 C17/C18 原始帧回归

当前候选为 **wire14a**，三份冻结源码包含 **4,385 个同字节项目文件**，三套 strict ALL 构建通过。下表为当前源码的专项验证，**尚未运行本候选的三套完整矩阵，0.3.0 强兼容仍未完成**。包版本仍为 **0.2.2**，Root HEAD 保持 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未 commit/push/release。

| 当前 wire14a 检查 | 实际结果 | 范围 |
| --- | --- | --- |
| Windows Release | **43/43 通过，零失败、零跳过** | 实际产物专项；注册总数 479，不是本轮全量结果 |
| 原生 Windows ASan | **44/44 通过，零失败、零跳过** | 实际产物专项；注册总数 480，不是本轮全量结果 |
| Linux Release/LTO | **55/55 通过，零失败、零跳过** | 实际产物专项；注册总数 699，不是本轮全量结果 |
| Linux 锁定 workerd 正式门禁 | **23/23 通过，零跳过** | 完整 fixture、独立客户端等原有检查，加上新的线路比较和验证规则控制；三个受支持 Host 后端 |

已正式接入两项 SDK 修正：关键四个翻译单元显式依赖生成的 `build_identity.h`；SDK 负向控制的 CMake 子进程使用 `ENCODING NONE` 保留原始诊断字节。此前 sdk13b 的专项为 Windows **29/29**、ASan **30/30**、Linux **29/29**，当前候选继续通过原往返、编译器/Worker/库身份、实际 SDK smoke 和六项消费控制。13b 归档中的旧 ASan Ninja 缓存为 0 个自动头文件依赖；**wire14a 重建后的两个 Windows 记录均包含身份头**，ASan 当前记录有 159 项依赖，显式构建边和实际编译器兼容 ID 也一致。旧缓存证据继续保留，不能据本次恢复推断通用 MSVC/语言环境问题已解决。原 13a 全量红灯未改写。

`js/websocket.js` 现在按分派顺序提交同步回调中已接受的发送/关闭操作，再处理回调抛错。独立 `websocket_throw_wire_*` 在 Windows Release/ASan 各两个后端、Linux 三个后端执行每后端八个连接，共 **56 个原始 TCP 连接**，逐字节断言 C17 的 `queued` 文本帧，以及 C18 的 `a-before` 文本帧后接 `1000/done` Close 帧；同时检查真实握手、失败监听器不继续执行、后续 HTTP 恢复、真实对端终止和正常 Host 停止。原始帧回归已加入 CMake、平台注册审计和独立必测清单，不能靠删除注册或添加跳过来通过。

正式 API fixture 也包含原始线路检查，基线实际执行 **748 条断言**。其比较器验证精确帧内容及受限客户端轨迹，保留实际 EOF/reset、消息和 close 事件结果。新增 **15 项验证规则控制**检查缺失/损坏/重复/乱序帧、超时/本地销毁、错误终止和错误客户端轨迹；门禁另拒绝双方一起丢失发送或 Close 的伪一致报告。实际 fixture 故障变体扩至 **19 项**：未改基线先通过，18 个变体均须以预先指定断言失败，包含新增的丢弃 C17 发送和破坏 C18 关闭码。旧产物在新的精确字节断言处失败，隔离修正及锁定 Linux workerd 在相同断言下通过；没有通过放宽为任意数组或任意关闭结果掩盖丢帧。

fixture 可执行行保持不变，仅纠正 C18 的旧注释；本候选 fixture SHA-256 为 `1677587be168720383a5194215afe8dbbf89a54e7caaca9710870ad5094a4ed2`。第 62 轮两个官方资产的 240 个连接、旧产物的 280 个连接和私有修正的 80 个连接仍按各自原始输入保留，不能充当本候选原生 Windows 完整门禁。

**传输边界仍开放**：已观测 Capsid C18 以 EOF 结束，而两个 workerd 官方资产以 TCP reset 结束；Node/undici 的已排队事件还会因平台和调度而不同。新门禁证明已接受的文本/Close 帧一致及连接有界终止，**不证明 TCP 结束方式或客户端调度完全相同**。原生 Windows 官方资产锁与完整门禁仍需接入，不能把 Linux 的 23 项结果记作 Windows 参考通过。

Windows 还有明确的注册缺口：`workerd_ws_gate_controls` 目前被 CMake 限定为 Linux，**不在 Windows 本轮 43/44 项 CTest 中**。准备阶段在 Windows 直接运行的门禁规则单测还跳过了既有 Linux-only 安装器控制；该直接试跑不是 Windows 参考通过。后续须接入原生资产锁、平台必测清单及这组控制的真实注册/执行，不能只调整二进制哈希后宣布覆盖完整。

最近三套完整矩阵仍属于 **13a**：Windows **476 项，459/5/12**，ASan **477 项，462/3/12**，Linux **695 项，690/2/3**（通过/失败/跳过），原始退出码均为 8。当前 SDK 专项已验证相关修正，但本候选全量、同源 TSan、qualified root 和 profile 最终复核仍待执行；旧 13a 的 9 项项目 TSan 与 root 3 项不能直接改名为当前通过。标准 WPT 原始失败、默认 workerd profile 边界、D004/D005、容量/资源/跨 shard/owner 组合、完整依赖 sanitizer/fuzz/soak、既定平台/交付及独立重建等余项继续按原计划处理。Hono 保持原应用、冷请求、500 ms 预算和既有 2 MiB 功能栈配置。

证据位于 `build-v030-dev/evidence/native-asan-qualification-20261018/`：`sdk13b-verification.json`、`wire14a-verification.json`、三套 `wire14a-*-targeted`/`wire14a-linux-focused` 日志、JUnit、LastTest 与注册表、`wire14a-linux-reference-gate/`、`wire14a-prebuild-controls.json` 和 `wire14a-preparation-incidents.json`。当前实际运行/构建输入归档 **74 个文件**，包括生成的 bootstrap JS/C/对象、实际二进制、身份头、关键对象、包及 Ninja 图；18 个受检源码文件单独保存。原失败、准备失误与历史观察均保留，本轮未作发布验收封存。

> 以下第 62 轮及更早内容为历史快照，当前修正、专项范围及未完成项以第 63 轮为准。

<!-- capsid-review-phase-62 -->
<a id="capsid-review-phase-62"></a>
## 第 62 轮当前进度（2026-10-18）：引擎修复全量验证与 C17/C18 线路反例

当前候选为 **13a**。三份冻结源码含 **4,382 个同字节项目文件**，Windows Release、原生 MSVC ASan、Linux Release/LTO 的 strict ALL 均通过；随后三套完整 CTest 均已执行并保留全部失败和跳过，**不能认定 0.3.0 已通过验收**。包版本仍为 **0.2.2**，Root HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未 commit/push/release。

| 当前 13a 检查 | 实际结果 | 范围 |
| --- | --- | --- |
| Windows Release | **476 项：459 通过 / 5 失败 / 12 条件跳过** | 原始 CTest 退出码 **8**，零排除 |
| 原生 Windows ASan | **477 项：462 通过 / 3 失败 / 12 条件跳过** | 原始 CTest 退出码 **8**，零排除 |
| Linux Release/LTO | **695 项：690 通过 / 2 失败 / 3 条件跳过** | 原始 CTest 退出码 **8**，零排除 |
| 三套框架/栈/引擎/overlay/注册专项 | **Windows 59/59、ASan 60/60、Linux 59/59，零跳过** | 实际 Worker、直接 C API 与 vendor 来源检查 |
| 同源项目 Debug TSan | **9/9，零报告、零跳过** | 核验 **103 个项目 C++ 输入**；完整依赖 TSan 仍未验收 |
| Linux 锁定 workerd 正式门禁 | **22/22，零跳过** | 全量归档后使用相同 Host/Worker 再执行；下述独立线路反例说明其覆盖仍不足 |
| 同二进制 qualified root | **3/3，零跳过** | 与当前 Linux 全量相同二进制 |

新增 `0044-quickjs-call-dispatch-and-regexp-captures.patch`，vendor patch 数为 **44**，已更新并验证 overlay 锁。原生调用在进入大型字节码解释帧前分派，共用入口仍保留一次中断检查；小型正则捕获使用局部数组，超过边界时保留原堆分配与清理路径。`src/`、`include/`、`js/` 及运行时编译选项相对 12a 未改。原 Hono 应用、首次冷请求、**500 ms** 请求预算和既有 **2 MiB Hono 功能栈配置**不变，三套完整矩阵中的生命周期用例均通过；默认及 256 KiB 的递归/恢复检查继续通过。这是当前机器与配置的验证，不是普遍性能保证。

新增实际 Worker 的八类调用/正则语义用例和直接 C API 中断用例；八类响应与 Windows、Linux 两个锁定官方 workerd 二进制均逐字节一致。私有反向控制分别引入捕获越界、错误释放栈数组、漏检原生调用中断，均由指定 ASan 报告或 C API 断言检出。错误副本和诊断脚本的准备失败另存，未替换正式对象或降低断言。

完整 Windows Release 暴露编译器身份不一致及相关 SDK 失败。其编译器对象的 Ninja 记录为 **0 个头文件依赖**，仍携带旧兼容 ID；用相同源码、当前头文件和原注册选项只刷新私有编译器对象后，原往返与 Worker/库身份两项实际测试均通过。新建最小 MSVC/Ninja 的头文件修改测试正常，**尚不能归因为通用 MSVC/语言环境缺陷**；后续须修复关键生成头的显式依赖并检查缓存依赖完整性。ASan SDK 负向控制实际已触发预期 `LNK2038`，但 CMake 解码损坏了 `annotate_string/vector` 符号，造成检查误报；仅在私有副本增加 `ENCODING NONE` 保留输出原始字节，原六项控制全通过。两项修正尚未合入本候选，正式全量红灯保留。

独立补查进一步发现 **C17/C18 真正的线路不一致**。锁定 workerd 的两个平台各执行四组、每组 30 次，共 **240 个连接**，其中 **120 个原始 TCP 观测**均收到 C17 的 `queued` 文本帧，或 C18 的 `a-before` 文本帧及 `1000/done` Close 帧。相同 Linux Node/undici 客户端却未上报这些消息，不能据此推断服务端回滚了发送。当前 Capsid 在 Windows Release/ASan 的两种后端及 Linux 三种后端共 **280 个连接**中，**140 个原始 TCP 观测的线路帧均为空**，另外 140 个 Node 事件观测也无消息。原测试把客户端事件丢失解释为服务端丢弃的依据不足；原有 22 项门禁通过不覆盖这一反例，**不能证明完整 WebSocket 强一致**。

私有 ASan Worker 仅把已接受的 staged send/close 按顺序提交后再处理同步抛错，两个后端共 **80 个连接**恢复了与两个 workerd 资产相同的文本/Close 帧；正式源码和原有断言尚未改变。下一步应先加入严格的 C17/C18 原始帧回归及反向控制，再修复提交语义、分开核验线路与 Node 事件观测，并完成原生 Windows 官方参考资产/门禁集成。不能仅把原 `[]` 改成任意可接受结果。

标准 WPT 的原始失败、默认 workerd profile 的既有边界和未命中的旧期待继续保留；profile 审计完成不代表标准全绿。容量/资源映射、替换重叠、503 待写、跨 shard 与 owner 生命周期组合、D004/D005、完整依赖 sanitizer/fuzz/soak、既定平台/交付及独立逐字节重建等余项按原计划继续。

证据位于 `build-v030-dev/evidence/native-asan-qualification-20261018/`：`engine13a-verification.json`、三套 `engine13a-*-full` 原始日志/JUnit/注册表、`tsan-engine13a-result.json`、`engine13a-linux-final-reference-gate/`、`profile-audit-engine13a.json`、`engine13a-fault-controls-v3/result.json`、`engine13a-additional-findings.json`。新的运行/构建输入已独立归档；六份结果文档在验证后更新并保存各自差异和哈希，本轮未封存。

> 以下第 61 轮及更早内容属于历史快照，当前范围、完整矩阵和线路反例以第 62 轮为准。

<!-- capsid-review-phase-61 -->
<a id="capsid-review-phase-61"></a>
## 第 61 轮当前进度（2026-10-18）：并发失败诊断与 Hono 冷启动耗时

当前候选为 **12a**，三份冻结源包含 **4,378 个同字节项目文件**，Windows Release、原生 MSVC ASan、Linux Release/LTO 的 strict ALL 均通过。本轮修改共享框架测试驱动及注册检查，新增真实 Worker 的失败诊断用例；`src/`、`include/`、`js/`、vendor patches 和运行时编译选项相对 11d 均未改变。包版本仍为 **0.2.2**，0.3.0 强兼容目标未完成。

| 当前 12a 检查 | 实际结果 | 范围 |
| --- | --- | --- |
| Windows Release | **52/52，零失败/跳过** | 全部已注册框架用例、JS 栈边界、注册与插桩审计控制 |
| 原生 Windows ASan | **52 通过 / 1 失败 / 0 跳过，共 53 项** | 唯一失败为原 `worker_hono_lifecycle`，原始 CTest 退出码 **8** |
| Linux Release/LTO | **52/52，零失败/跳过** | 同类框架、栈及审计专项 |
| 同源项目 Debug TSan | **7/7，零报告/跳过** | 新诊断、栈与审计；核验 **102 个项目 C++ 输入**，不是全部依赖资格 |
| Linux 锁定 workerd 正式门禁 | **22/22** | 当前相同 Host/Worker 二进制；不代表原生 Windows 参考完成 |
| 私有 ASan 诊断反向控制 | **1 基线通过，3 个错误副本被指定断言检出** | 错报 owner、错报超时类型、丢失原始 payload；原旧驱动 RED 另存 |

共享驱动原先把所有并发错误压成 `concurrent framework request failed`，掩盖了实际请求与原因。现在保留 request ID、ERROR/REQUEST_TIMEOUT、flags 和完整 payload。新增 `worker_framework_concurrent_diagnostics`，直接运行真实 Worker，分别检查正常并发、第一/第二请求抛错、第一/第二请求超时五个场景；同时核对错误栈、完整正常响应和驱动退出。普通错误不能冒充超时，固定报错的假驱动也不能通过健康场景。独立注册契约要求此用例的实际驱动、Worker、fixture、命令和超时，元数据控制覆盖三个平台与两种 WPT 配置的六种组合，共 **7,444 个错误变体**；这些元数据控制不算产品执行。

Hono 的原 **500 ms** 请求预算、冷启动、首次慢/快请求、所有断言和功能栈设置保持不变。当前正式 ASan 原始日志明确为：`request_id=1001 event=REQUEST_TIMEOUT flags=1 payload=request deadline exceeded`，并重新捕获 `InternalError: interrupted` 在 `insert` / `buildMatcherFromPreprocessedRoutes` / `buildAllMatchers` 的栈。此前仅有 generic FATAL 时对当前具体栈的保留意见，已由本次同源正式日志补齐。

私有同源测量将首个作业约 **510–524 ms** 的耗时归到同步冷路由构建，线程 CPU 计时也显示主要在执行。内建调用采样进一步记录 matcher 构建与排序；这不是单独证明“截止时间判断错误”。私有 `/Ob2`、`/O1`、`/Os` 与拆分内建调用入口的试验均未使原生命周期测试通过；入口拆分只把测量降到约 **504–507 ms**，仍不足。**这些引擎/编译改法未合入，不增加预算、不请求外预热、不关闭 ASan 或 STL 注解，也不删除失败项。** 这些时间只描述当前机器和插桩配置，不作普遍性能保证。

候选 11d 的真实 4,096 字节 ACK 队列压力证明、完整 1 MiB filler/终端/自然退出检查，10e 的 Supervisor 持有与截止时间修复，以及 SDK 的实际包消费修复继续保留。它们的 46/47/84、81 项 TSan 等数字属于此前专项；本轮新范围见上表。最新零排除完整矩阵仍为历史候选 **8b**，当前 12a 未完成普通/ASan 全矩阵和同源 qualified root/profile。标准两个 WPT 失败、Windows pinned workerd C17 调度差异与其他第 59 轮未关闭任务继续保留，不能把 profile 分类解释为标准全绿。

接着执行：定位冷路由的 JS/native 调用及分配/ASan 栈成本，在同一个应用、首次请求和预算内解决后再验证；不采用本轮未成功的私有试验作为修复。之后继续原生 Windows C17 的精确调度/反例门禁、最终源码的完整普通/ASan/root/reference/profile、资源与 owner 生命周期组合、D004/D005、完整依赖 sanitizer/fuzz/soak 及既定平台/交付项。

证据位于 `build-v030-dev/evidence/native-asan-qualification-20261018/`：`framework12a-verification.json`、三套实际 CTest 原始日志/JUnit/注册表、`tsan-framework12a-result.json`、`framework12a-linux-reference-gate/`、`framework12a-fault-controls/result.json`、`hono12-diagnostics-original-red.json` 与各 `hono12-*` 私有观测。运行/构建输入已独立归档，准备失败另记 `framework12a-operator-incidents.json`。六份结果文档在验证后写入，差异及归档记录为 `result-documents-framework12a.json`；本轮证据尚未封存。Root HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未 commit/push/release。

> 以下第 60 轮及更早内容属于历史快照；当前范围和 Hono 具体原因以第 61 轮为准。

<!-- capsid-review-phase-60 -->
## 第 60 轮当前进度（2026-10-18）：出站 ACK 压力前提与真实 Worker 夹具

当前代码为候选 **11d**，三份冻结源含 **4,376 个同字节项目文件**，Windows Release、原生 MSVC ASan、Linux Release/LTO 的 strict ALL 均通过。本次纠正了出站压力测试没有证明“ACCEPT 已读取、结果 ACK 因 wire 队列已满而推迟”的问题，新增独立用例及插桩核验。没有修改 WebSocket 的 JS 语义或放宽事件断言；包版本仍为 **0.2.2**，0.3.0 强兼容目标未完成。

| 当前 11d 检查 | 结果 | 范围 |
| --- | --- | --- |
| Windows Release 专项 | **46/46，零失败/跳过** | 出入站 WebSocket、ACK/响应头压力、Supervisor、池/executor/capacity、实际包消费与注册 |
| 原生 Windows ASan 专项 | **47/47，零失败/跳过** | 同类实际运行及插桩核验 |
| Linux Release/LTO 专项 | **84/84，零失败/跳过** | 另含 managed 恢复/回收、Binding、栈与 metrics |
| 同源项目 Debug TSan | **81/81，零报告/跳过** | 核验 **102 个项目 C++ 输入**；完整 vendor/更广依赖仍未完成 |
| Linux 锁定 workerd 正式门禁 | **22/22** | 当前同一 Host/Worker 二进制；原生 Windows 参考仍未整合 |
| 私有原生 ASan 控制 | **1 基线通过，4 个真实缺陷变体被拒绝，1 个本用例可观察等价变体通过** | 原始存活变体完整保留，不算作被检出的错误 |

旧测试在当前 10e ASan 二进制上仍实际失败。它发送 16 个 filler 后固定等待 100/250 ms，不能证明 close 前 ACK 没有进入输出队列。新夹具编译普通 Worker 的全部 **12 个源输入**，只追加专用入口；启动、bootstrap、JS 引擎、协议和队列实现都复用原代码。四种配置的实际对象/编译输入核验确认，公共源字节相同，预处理定义仅多测试入口选择宏；共享 runtime/main 和夹具 main 的每一份编译项均核验对应 ASan/TSan 参数，新增 2 项元数据反向控制，没有关闭 STL 注解。夹具不安装到发布包。

受控 writer 先把真实 head 和首段 body 交给 socket，再对下一帧返回 EAGAIN。Worker 实际观测到队列 **4,096/4,096 字节**、assigned owner 尚未 committed、结果 ACK 无法排队；原 **200 ms** JS close 产生 assigned CANCEL 后才恢复写入。四种配置的完整/独立用例各有两份实际状态记录，均为唯一结果 ACK、唯一 assigned CANCEL，没有 DATA/open Close。普通握手、拒绝/交叉取消、20 次 orphan 与两槽容量复用仍执行普通 capsid-worker；16 owner 的 v3/v4 混合响应头压力也保持不变。

压力填充改为一个真实 1 MiB 响应，由队列满态直接证明前提，同时检查全部字节、唯一 head/RESPONSE_END、后续 healthy 响应和 sole reaper 的自然零退出，强于原先忽略 filler 完成的断言。原 **10,000 ms** 请求预算、**5 s** 事件截止和 **30 s** CTest 超时未变。新增 `worker_outbound_result_ack_pressure`，注册审计同时要求完整套件传入实际普通 Worker 和夹具路径，不能漏掉压力分支。

四个被拒绝的变体分别关闭受控 stall、漏记 ACK 阻塞证明、漏发 assigned CANCEL、提前分发 open。其中漏记证明的变体即使正常协议流程完成，夹具仍以实际退出码 **73** 使测试失败。删除 native pending-open 清理的变体在本用例仍通过，因为 JS 的 CONNECTING/已 close 保护继续抑制 open；它按可观察等价控制保留，不宣称覆盖其他生命周期组合。原型漏配完整 filler 的响应 credit 导致的 5 s 失败、主函数重构后的返回值编译警告，以及私有链接/输入审计路径准备纠正都另行保留，没有当作产品 RED。

候选 10e 的 Supervisor 两层持有、阻塞读取后的实例/停止校验、显式线程 join 与替换截止时间毫秒转换修复继续保留，七项真实回归仍在当前套件执行。候选 9 的 SDK Config/版本入口、导出 ASan 参数、三项 DLL 的身份/清单/SBOM、移位后实际 C/C++ 与 Host 消费也已在本轮相关范围重检。独立重建目前核验确定性文本与身份并记录二进制差异，尚非全部二进制逐字节复现。

最新的零排除完整矩阵仍属于历史候选 **8b**，不能当作当前 11d 全量：

| 候选 8b 完整矩阵 | 通过 | 失败 | 条件跳过 | 原始退出码 |
| --- | --- | --- | --- | --- |
| Windows Release，464 项 | 450 | 2 | 12 | 8 |
| 原生 Windows ASan，465 项 | 448 | 5 | 12 | 8 |
| Linux Release/LTO，683 项 | 678 | 2 | 3 | 8 |

8b 的 qualified root **3/3**、标准 WPT **84 文件 / 14,060 子测试（82 接受、2 拒绝）**和严格 profile 审计保留为历史结果。profile 只分类既有 3 条 workerd 默认行为冲突，并保留 1 条未命中的旧期待，不能解释为标准全绿。当前 11d 未执行完整普通/ASan 矩阵或新的同源 root/profile 配套资格。当前源码的 ASan Hono 生命周期已再次实际复现原 **500 ms** 预算内失败，证据为 `hono12-native-asan-baseline.json`；不能因出站专项转绿而关闭它。

继续项按顺序为：

1. 测量并解决当前原生 ASan Hono 首次并发请求中 matcher 构建期间的实际中断；保留 **500 ms** 预算和冷启动语义，不以增大预算或请求外预热关闭。2 MiB ASan 功能栈与默认 1 MiB 边界分别保留。
2. 原生 Windows pinned workerd 的 C17 调度差异需精确反例与正式平台门禁；现有 10 次采样不能替代正式参考通过，严格期待未放宽。
3. 完成最终源码的零排除普通/ASan 全矩阵及同源 root/reference/profile，保留标准两个 WPT 失败和未关闭问题。
4. 容量/资源映射、替换重叠、503 待写、跨 shard 与 owner 生命周期组合、D004 时钟、D005 HTML、完整依赖 sanitizer/fuzz/soak、native Mac/Hosted/T15–T17 与独立逐字节重建按第 59 轮余项继续。

证据在 `build-v030-dev/evidence/native-asan-qualification-20261018/`：`outbound11d-verification.json`、`outbound11-native-asan-original.*`、`outbound11b/11c-native-asan-probe*`、`outbound11d-*-targeted.*`、`outbound11d-linux-focused.*`、`tsan-outbound11d-result.json`、`outbound11d-fault-controls-summary.json`、`outbound11d-*-worker-inputs.json`、`outbound11d-linux-reference-gate/` 与 `progress.json`。二进制、库、包、消费者和插桩输入单独归档；历史失败保留，本轮未封存。Root HEAD 仍为 `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f`，未 commit/push/release。六份结果文档在验证后写入，差异记录为 `result-documents-outbound11d.json`。以下较早记录按各自快照理解。

<!-- capsid-review-phase-59 -->
## 第 59 轮当前结论（2026-10-18）

已修复 IPC metrics 采样持有期、退出后末段真实计数及 Windows uint64 格式，补 4 个 metrics 用例和 2 个注册控制。Windows 必要注册 103 个、Linux 117 个，控制拒绝 5,924 项元数据篡改。完整结果 **Windows 455：441 通过 / 2 失败 / 12 跳过；Linux 674：669 通过 / 2 失败 / 3 跳过**，均零排除、原始 exit 8。Linux Debug TSan **61/61、零报告、零跳过**，workerd 必选门禁 **22/22**、qualified root **3/3**。两份标准 WPT 仍各有 2 个失败文件，严格 Linux profile 接受不等于普通套件全绿。

Windows ASan 新配置成功但 ALL 因 Ada STL 注解不一致而链接失败，运行时资格尚未通过；现有 instrumentation gate 另有 MSVC 参数误拒绝。下一步先修这两处实际失败，再复现 Supervisor pool/executor 持有期并补原生 Windows reference/Hosted WPT。物理容量、503 待写/跨 shard 生命周期、D004/D005、完整依赖 sanitizer/fuzz/soak/native Mac/Hosted/T15–T17 与独立逐字节重建继续保留，包仍为 0.2.2，整体目标未完成。

详细边界、初版测试纠正及原始 RED 见[审校第 59 节](v0.3.0-glm-takeover-review.md#capsid-review-phase-59)和 `build-v030-dev/evidence/ipc-metrics-ownership-20261017/verification.json`、`follow-up-plan.md`。当前冻结源 4,361 文件同字节，六份结果文档最后写入；Root 未 commit/push/release。下面较早轮次的记录保留为历史。

## 第 57 轮验收（2026-10-17）：quarantine 顺序、代际校验与进程容量

最新证据 `build-v030-dev/evidence/managed-quarantine-ordering-20261017/`，详细记录见[审校第 57 节](v0.3.0-glm-takeover-review.md#57-2026-10-17-quarantine-顺序代际校验与进程容量审校)。本轮完成 quarantine/部署交错、retired 回收和对应容量记账修复；**Linux/Windows 的 0.3.0 强兼容目标仍未完成**。验收边界逐项记录在 `goal-requirements-audit.json`。

1. pool 不再在 mutex 内执行 quarantine 持久化。pump 仅在锁外向管理线程提交弱引用任务，管理线程取得 AppOperationLock 后先检查已发布 pool 的所有权及 expected generation，再读取/写入活动状态。同一 generation 重新激活仍按实例区分；已中止/已替换的任务不会误写新代 tombstone。任务不持有 pool 强引用，避免中止部署在 App 锁内销毁 pool 时又等管理任务。正常 quarantine 等待持久化尝试结束再停健康 Worker；显式 owner drain 可结束已中止的 pool，不依赖管理线程先取得锁。
2. `fleet_exited()` 同时检查 slots 和 retired executor。drain-complete 和容量释放必须等所有实际进程完成 reaping；只收到 EXIT 通知或换下 slot 不足以释放。便携回归暂停原 executor 的退出通知，以真实 OS 状态验证边界，Windows 也执行该内部组件测试。
3. 每个 pool incarnation 用独立 capacity lease 记录 prepared/active/draining/released；提交先结算计数类别，再发布并排空旧 pool，最终 reap 按正确类别仅释放一次。修复 active quarantine 向空 surge 计数扣减导致回绕和后续 App 无法部署的问题。prepare 返回空或事务配置不完整时也归还启动预留；失败持久化先等新进程回收再归还。另补 retired 超出名义 surge 后的绝对容量检查，以及 UINT64_MAX 加减边界。继续审校发现 stale guard 在 RAII 接管前返回会泄漏目录句柄；新增真实回归证明旧候选 100 次跳过泄漏 100 个句柄，修复为取得 App 目录后立即交给 RAII，最终 100 次守卫不增加任何该目录句柄。

新增 **Windows 8 项、Linux 21 项 CTest**：两个 pool 顺序/回收回归、两个计数边界、四个 lease 状态/并发回归，Linux 另有九个 quarantine 事务、两个启动失败和两个真实 Host 可执行程序场景。覆盖旧代、同代不同实例、prepare commit/abort、退役、真实文件写失败、排队停止、过期通知，以及部署持锁时旧 pool 达到 quarantine 阈值。写失败通过 READY 后施加 RLIMIT_FSIZE 触发真实 EFBIG，不靠伪造返回值。新真实 Host 用例验证实际 HTTP 200/精确正文和重启恢复。

专项 **Windows 25/25、Linux 55/55**，均零跳过。新增两个 pool 用例每轮有 **4 个自然 OS code 0 退出**；Linux 新增的事务用例合计另有 **19 个自然 OS code 0 退出**，共 23 个，并记录完整响应。一个中止启动仅能确认产品已 reap、退出原因不可得，明确保留 `exitStatusAvailable:false`。新真实 Host 正向用例没有夹具代清理；驱动只对自己的后代启用 subreaper，正常 Host 停止后发现遗留子进程即失败，有意中止 Host 或失败夹具的清理另记 `FIXTURE_CHILD_CLEANUP`。

候选 5 的 663 项全量另有 `websocket_managed_replacement-recursion` 失败，原始 exit 8 保留。该旧夹具排除了僵尸并以可见 Worker 数代替 generation 完成；从锁序、计数类别和失败阶段判断，下一次部署可能早于实际容量归还；历史 Admin 响应只保留 failed，未返回具体错误，因果解释属代码与日志推断。最终候选计入尚未 reap 的全部自有子进程，并等待在实际回收/lease 归还后发布的 `worker_events_total{event="generation_drained"}`，再开始下一轮部署；不重试失败操作、不加固定延时、不放宽 RSS/VmSize 断言。本轮循环针对已激活旧代；prepared/abort 预留仍由协调器在中止完成时结算，不能仅凭该事件推断所有未提交预留已清空。旧 Host 缺少完成观察时的新夹具对照失败，修复后 16 轮实际部署、响应、回收及完成事件均成立。

旧实现 RED 与最终输入对照均保留：两个 pool、真实 Host 容量、两个纯记账边界先在旧产品运行并失败；stale guard 目录句柄测试在候选 4 上也命中 100 个泄漏并失败；另以最终测试源码、最终真实 Worker 做 **13 对旧/新输入组合，共 26 次运行**，旧实现均命中失败断言、修复后均通过。旧协调器仅加签名适配以链接新驱动，仍保留旧的无代际 quarantine 行为；两个 pool 回归分别使用旧/新产品编译的驱动，测试源码相同；可执行程序场景共用最终驱动并替换 Host；协调器采用签名适配；其它组合的具体输入逐项记录。私有源码、编译/链接命令、库成员替换、各输入 SHA 和全部输出可复核，不声称所有组合共用同一驱动二进制。负向失败清理不算自然退出，固定输入及完整套件归档校验未变。准备期间两次错误路由和一次错误 singleflight 并发假设的夹具失败原样保留，见 `preparation-notes.md`，未计为产品 RED。

| 验证 | 实际结果 |
| --- | --- |
| Windows strict Release 全量，零排除 | **355：343 通过、0 失败、12 条件跳过** |
| Linux strict Release/LTO 全量，零排除 | **663：658 通过、2 个 WPT 文件失败、3 条件跳过**；保留 CTest exit 8 |
| 当前 Debug TSan | **54/54**、零报告、零跳过；核验 82 个项目 C++ 编译输入，含真实 Host main、新 dispatcher、账本/lease 驱动和 Worker Runtime |
| 固定 workerd 正式门禁 | **22/22**，同一普通 Linux 二进制，零跳过 |
| 同一普通 Linux 二进制 qualified root | **3/3**，零跳过 |
| 标准 WPT | **84 文件/14060 子测试**；原始断言、期待及失败不改 |
| 补充原生 Windows WPT 执行 | **84 文件/14060 子测试，81 报告接受、3 拒绝**；原生 Node 逐个核验 111 份上游文件，使用字节相同的便携 JS 包；不计入原生 CTest |

本轮失败文件：`worker_wpt_html_webappapis_scripting_processing_model_2_unhandled_promise_rejections_support_promise_rejection_events_js`, `worker_wpt_webmessaging_MessagePort_initial_disabled_any_js`。完整运行另有 69905 条实时钟/单调钟采样，最大相邻 offset 变化 4321.004 ms；保留原 30 ms 断言，未额外重跑覆盖结果。 严格 profile 校验成功，只分类固定版本的已知冲突；普通全量仍失败，不能改称标准 WPT 通过。 固定 workerd v1.20261010.1、兼容日期 2026-08-20、flags=[] 和 SDK 锁不变。默认 vendor C、更广依赖和原生 Windows 未纳入本轮 Linux TSan 结论。

Native `7129964d2505bd5e6943cefd6925ee23b187dbc6`、Linux `3a0dbf9cd78f7337cc780bcab7509d0bb893cbb1` 的 **4349** 个项目文件字节相同；本轮产品/测试变更 17 文件，六份结果文档最后写并单独归档。根 HEAD `8028b88adff26cbc1abcb9a2fafce2ffc3d8090f` 不变，保留全部既有修改，无根仓库提交/推送/发布。公开 C ABI、C Runtime、IPC EXIT、产品 JS、43 个 overlay、WPT 原始断言和 reference/SDK 锁未改。证据包括 `verification.json`、`implementation.diff`、`private-controls6-result.json`、`tsan4-result.json`、`profile-audit4.json`、`final-audit-review6.json`；前十七份封存逐文件复核，最终封存后只读。

补充原生 WPT 发现 **GB18030 的 18 项平台差异**：Windows 的 275 项 GB18030 原始断言全部符合；Linux 有 18 项 GB18030-2022 断言失败并被 D011 接受。使用未改的预期失败元数据时，Windows 因这 18 项“缺少预期失败”而拒绝报告，另外两个被拒绝文件仍是上述事件差异。完整补充报告保留 **2 expected / 3 unexpected / 19 missing expected**。这组 JS 包由 Linux 的现有 fixture build 生成，原生 Windows 实际执行与上游 blob 审计均完成，但原生 WPT 生成流程和 CTest/Hosted 注册尚未验收；`native-wpt-supplemental6/analysis.json` 保留逐文件差异，不据此宣称标准 WPT 全通过。

接下来优先补 **原生 Windows WPT 构建/注册、workerd 对照、MSVC ASan**，固定 workerd 已用同一完整 GB18030 包实测 275 子测试、0 实际失败、18 missing expected，与 Windows 相同（见 `workerd-gb18030-probe2/result.json`）；接着修正 Linux 现有 JS 解码器的四字节 pointer 别名，再在两平台实际通过后移除 D011；不能通过放宽期待隐藏差异。当前 Windows 全量未设置 WPT_ROOT，未注册任何 worker_wpt_*，两个 reference 用例条件跳过；Linux 的 22 项门禁不能替代该缺口。按现有平台契约，共同范围包括 Runtime、Worker、single-worker 和 static-pool；Managed coordinator 仍只支持 Linux。另需复现并修复 SingleWorkerServer 启用 IPC metrics 时借用 handle 与 reaper 的竞态，并为本轮新增用例补删除/替换命令的注册控制。末尾源码审校另发现 Supervisor 的 current_pool provider 返回裸 pool，supervisor 又跨 health-config 读取持有裸 executor；reclaimer/finalize_drain 可并发释放对象。该项尚未受控复现或修复，属于代码持有期推断，不能用本轮 TSan 零报告排除，见 `follow-up-audit.md`。

503 待写/写超时、跨 shard/post/owner exit/stop、池级非零配置、replacement 瞬时真实进程数和资源矩阵、更多真实 Host 双故障、稳定时钟 WPT、D005 文档 HTML、三个既有 workerd 诊断差异仍待处理；完整 vendor sanitizer/fuzz/soak/native Mac/Hosted/T15–T17 和全新独立逐字节双构建亦未关闭。T02/T04/T06–T08/T10–T17 不作整项完成。

## 第 56 轮验收（2026-10-17）：Managed 进程观察与替换回调生命周期审校

最新证据 `build-v030-dev/evidence/managed-process-observation-20261017/`，详细记录见[审校第 56 节](v0.3.0-glm-takeover-review.md#56-2026-10-17-managed-进程观察与替换回调生命周期审校)。本轮修复两个产品生命周期问题，新增三项跨平台回归；0.3.0 总体仍未完成。

1. Managed 主程序曾把借用的 `capsid_worker*` 保存在观察 map 中。新代准备就覆盖监控 PID，回滚又可能恢复已被唯一 reaper 释放的旧指针；退出回调取到 nullptr 时也无法匹配并清掉旧条目。现在 executor 在线程启动前缓存 PID，监控仅从已发布的路由快照选择可用进程，删除该 map 和 ActivationPlan 的旧指针快照。准备、回滚和旧代退出无须写第二份监控状态。多个 App 按路由名确定性选择首个可用进程；这是现有单条 Worker RSS 观察，不是全舰队聚合。
2. replacement 在调用 `on_worker_started` 前就递减 in-flight 计数，排空可能提前释放 callback 所用 executor。现在计数持续到回调返回，保持回调在 pool mutex 外执行。受控测试暂停真实 replacement 的回调，确认 pump 已处理其 EXIT，再检查排空不能先完成。仅改测试的旧产品快照 RED 为 exit 1，修复后各平台均通过；另用完全相同的测试源码和最终真实 Worker 对照旧/新驱动，仍为 **exit 1/0**，源码、驱动和 Worker SHA 均核验，未改全量证据。RED 验证此生命周期，不声称捕获旧 Managed 指针的完整 UAF 现场；最初 RED Worker 未及时归档的边界保留，见 preparation notes 和 `same-input-controls.json`。

新增 `host_generation_replacement_observer_drain`、`host_process_observation_publication`、`host_process_observation_replacement`；增强既有 adopted executor 的 PID 保留断言。覆盖准备/回滚、提交后旧代退出、多 App 退役回退、并发发布/读取和健康替换。每轮新增三项均记录 **8 个实际自然 OS code 0 退出、4 个完整 200/精确正文响应**。进程观察测试还通过 Windows 独立进程 handle 或 Linux 产品 reap 后的 ECHILD 验证回收，不能靠测试自己收走僵尸取绿。原生 Windows 仅验证可移植 Host 内部组件，未新增 Managed coordinator 平台支持。

Windows strict Release 全量 **347：335 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **642：637 通过、2 个 WPT 失败、3 条件跳过**，均零排除。Native 专项 **13/13**、Linux 专项 **24/24**、当前 Debug TSan **16/16**、固定 workerd **22/22**、qualified root **3/3**，专项均零跳过。TSan 选定编译闭包含 76 个项目 C++ 输入，包含真实 Host main、新驱动及 Worker Runtime；实际运行真实 Host metrics endpoint。默认 vendor C 和更广依赖仍未完整插桩，无 Windows sanitizer、native Mac 或 Hosted CI 通过结论。

标准 WPT 仍 **84 文件/14060 子测试**，失败文件为：`worker_wpt_html_webappapis_scripting_processing_model_2_unhandled_promise_rejections_support_promise_rejection_events_js`, `worker_wpt_webmessaging_MessagePort_initial_disabled_any_js`。本轮保留完整实时钟/单调钟独立采样，未改 HR-Time 的原 30 ms 容差，也未额外重跑以覆盖全量结果。 本轮未出现第 55 轮的 HR-Time 失败；这不替换第 55 轮 639 项完整失败，也不证明该环境的时钟始终稳定。 严格 profile 核验成功；这只分类固定 profile 的已知冲突，不是普通套件或标准符合性通过。 普通 Linux 全量仍失败。ABI、C Runtime、IPC EXIT、43 个 overlay、JS/WPT 原始断言、reference/SDK 锁均未改。首次 MSVC 严格构建的局部弃用诊断失败完整保留，最终修正只对已有 shared_ptr 原子兼容调用作局部诊断处理，未取消 `/WX`。

Native `070ece00c021aa49f24f36ab30b01983b47089d9`、Linux `b8a6f639814f563b400c5f6ce83a1cee980c52b4` 的 **4346** 个项目文件字节相同。最终验证前冻结 14 个本轮变更文件，六份结果文档最后更新；根 HEAD 不变、既有脏修改保留。证据汇总见 `summary.json`、`verification.json`、`red-result.json`、`tsan2-result.json`、`final-audit-review.json`、`artifact-digests.json`；前十六份封存保持原样。

下一轮先受控核验三个代码观察点：quarantine 回调在 pool mutex 内等待 AppOperationLock，而部署持有 AppOperationLock 调旧 pool 的 drain，静态锁序成环，且回调没有 expected generation 条件；SingleWorkerServer IPC metrics 仍借用 worker handle，可能和 reaper 重叠；`fleet_exited()` 只检查 slots，retired executor 延迟退出时 drain-complete/容量释放是否提前。具体复现和方案边界见证据 `follow-up-audit.md`，均未计本轮关闭。随后继续真实 Host/其它驱动的 OS 状态、并发 deploy/retire/replacement/取消、503 待写/超时、跨 shard/post/owner exit/stop、稳定时钟 WPT、D005 HTML 与三个既有 workerd 差异，以及 Windows ASan、完整 vendor TSan/fuzz/soak/native Mac/Hosted/T15–T17/全新逐字节复现。T02/T04/T06–T08/T10–T17 不作整项关闭。

## 第 55 轮验收（2026-10-17）：Managed 生命周期响应与实际 OS 退出审校

最新证据 `build-v030-dev/evidence/managed-exit-observations-20261017/`，详细记录见[审校第 55 节](v0.3.0-glm-takeover-review.md#55-2026-10-17-managed-生命周期响应与实际-os-退出审校)。Native `80c556484dceb937036552040fce15dffa824d73`、Linux `c91c4db2e554f69d9d9b10e24e9a2a331999b8ad` 的 4345 个项目文件字节相同。本轮只改 `tests/test_host_managed.cc`、`cmake/build_tests.cmake` 两个项目文件，新增五项 Linux CTest；产品 C++、ABI、Runtime、43 个 overlay、reference/SDK 锁及 WPT 原始判定保持原样。

旧请求辅助函数只等 inflight 清零，Worker 异常退出也能满足；旧 replacement 检查可能读到退出处理前的旧 READY=3。本轮要求实际完整 200 响应、精确正文与目标 executor，明确终止观察到的 Worker，等待原目标退出及独立新 Worker 恢复容量。两个受控输入（错误正文、未发生替换）在旧驱动均 exit 0，在加强后的驱动均 exit 1；完整私有源码、编译/链接命令、输出与受限子进程清理记录保留。

新增单/三 Worker Admin generation 替换、单/三 Worker 请求取消、三 Worker health recycle 到 durable quarantine。既有 replacement、retire、persist 失败回滚与幂等重新激活补上实际 OS 状态；在 completed-drain 回调中复制状态，避免 executor 销毁后读取。每轮九项 Managed 回归观察 **27 个退出：26 个自然 code 0、1 个真实 SIGKILL**，并核验 **15 个完整响应**；产品独占 reaper 完成后，测试只以 ECHILD 确认已回收，不能靠测试自己 reap 僵尸取绿。取消须由同一实际 Worker 继续响应，不能伪装为退出、替换或 quarantine。

新 Debug TSan 构建暴露既有 `test-host-managed` 漏链 SingleWorkerServer 实现的问题，Release 优化曾掩盖它。已按实际 Boost 条件补齐测试目标的实现与链接依赖；GNU Boost.Beast 警告例外沿用 capsid-host 的范围。修正前的全量运行主动中断并单列保留，不计验收；final2 从新快照完整构建和重跑。最初 optional 访问编译错误、私有驱动 include 准备失败及所有历史尝试见 `preparation-notes.md`。

最终 Windows strict Release **344：332 通过、0 失败、12 条件跳过**；Linux strict Release/LTO **639：633 通过、3 个 WPT 文件失败、3 条件跳过**，全量均零排除。失败包括原来的两个文件及本轮 HR-Time basic。Native portable adoption 专项 **9/9**、Linux Managed 专项 **9/9**，均零跳过。新快照 Debug TSan **9/9、零跳过、零报告**，核验 67 个选定项目 C++ 编译输入含 `-fsanitize=thread`，涵盖驱动、Managed host core 与 Worker Runtime；默认 vendor C 和更广依赖仍未全量插桩，无 native Windows sanitizer 结论。

固定 workerd 门禁 **22/22**、qualified root **3/3**，均零跳过。标准 WPT 仍为 **84 文件/14060 子测试**。本轮 HR-Time 测得 performance 约 1996.206 ms、Date 约 4873 ms，相差 2876.794 ms；独立 Python 采样记录 realtime/monotonic offset 突跳 2876.701 ms，差值小于 1 ms。这支持环境时钟不连续的解释，原 30 ms 容差和本次失败均保留。一次独立 HR-Time 诊断 exit 8，只作诊断，不能替换 639 项全量判定。由于额外标准失败，独立 profile 核验按设计拒绝，`profile-audit2.json` 保留 `status:failed`、`complete:false`；本轮没有成功 profile 分类，也没有普通套件或标准符合性通过结论。Hosted CI 未实际执行。

此轮关闭上述九项回归的响应与退出观察缺口，且修复对应 Debug 测试链接；不代表 Managed 全路径或 0.3.0 整体验收完成。真实 Host 可执行程序/其他驱动的更广 OS 状态、并发 deploy/retire/取消组合、503 待写与跨 shard 排列、D005 HTML、三个既有 workerd 差异、Windows sanitizer、完整 vendor TSan/fuzz/soak/native Mac/Hosted/T15–T17 和全新逐字节复现继续保留。T02/T04/T06–T08/T10–T17 不作整项关闭。

## 第 54 轮验收（2026-10-17）：workerd WPT profile 精确映射与证据核验

最新证据 `build-v030-dev/evidence/workerd-wpt-profile-20261017/`；详细审校见[第 54 节](v0.3.0-glm-takeover-review.md#54-2026-10-17-workerd-wpt-profile-精确映射与证据核验)。Native `8511dbd9944dacf5605baee9b00b6bc64feeebef`、Linux `4e77cd6f2139eb7c6dc1c0cc91cc31fb11032590` 的 4345 个项目文件字节相同。本轮九个项目文件变化，其中三个新文件；各平台新增两项声明/负向控制 CTest。标准 WPT 仍为 84 文件/14060 子测试，原两个文件失败保留。根 HEAD 和既有修改保留，六份结果文档最后写。

新增 `tests/wpt/workerd-default-profile.json`，版本为 `CAPSID-WORKERD-v1.20261010.1-default-v1`，绑定 workerd release/source/binary SHA、compatibility date/flags、WPT commit 和两个原始源文件 SHA。独立工具 `tools/workerd-wpt-profile.mjs` 将三条精确标准失败与四后端真实 API 观察对应；原来的标准判定器、21 条 expectation、事件发生 guards、C ABI、Worker Runtime、全部 C++ 产品源码、43 个 overlay patch 和 reference/SDK 锁保持原样。

核验器要求完整普通套件的 LastTest、JUnit、注册清单，与同构建的正式门禁注册清单一致；核对当前 Host/Worker、驱动和两个实际生成 bundle 的身份。三个失败的子测试名、test 阶段、异常类型和诊断首行必须精确匹配，D010 的未命中旧期待也必须仍存在。四后端分别要求 handled 事件及 promise 身份、原 unhandled reason 身份、capture 注册拒绝和 start 前后独立队列观察。清理阶段失败、超时、缺事件、局部文件、额外失败和丢失对照均不能套用此分类。

成功分类为 **`profile-matched-with-standard-failures`**，同时保留 **`standardCTestFilesPassed:false`、`ordinarySuitePassed:false`、`standardsConformanceClaimed:false`**。它覆盖两个标准文件与对应的四后端观察；不作全部 Web API 一致或标准符合性结论。输出路径检查保护输入文件、硬链接别名、原门禁归档和 CTest 状态；失败尝试会使已有本工具成功报告失效。

CI 先保留普通全量注册与原始判定，即使该步骤失败也归档完整 LastTest/JUnit 并运行独立 profile 核验；普通步骤的失败继续阻断 workflow。工作流审校增加禁止跳过/忽略普通全量失败、禁止过滤标准 WPT，以及要求归档、绑定和上传 profile 证据的控制。新加过滤正则首次漏拦 `-E` 被真实私有工作流控制检出，已修复并保留 RED；该定位输入没有放宽，后续另加跳过控制，所以不宣称整份 RED 测试源码与最终文件字节相同。最初错误负向 fixture 和 CTest footer/label 解析准备问题也保留在 `preparation-notes.md`。

Windows strict Release 全量 **344：332 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **634：629 通过、2 个既有 WPT 文件失败、3 条件跳过**，均零排除。专项 Windows **10/10**、Linux **16/16**；两平台各执行 profile 控制 **57 项**、工作流控制 **44 项**。固定 workerd 门禁仍为 **22/22、零跳过**，qualified root **3/3、零跳过**。四后端精确帧各四连接/56 帧，Native 两模式各四连接/56 帧、SDK 各 22 场景。Hosted CI 未由本地控制实际执行；本轮未重跑 TSan，第 53 节 86 项/100 个 C++ 输入的结果仍是独立历史证据。

另以同一实际 Worker/C++ 驱动执行完整 36 子测试文件：正常 bundle 保留两个原 reason 失败并匹配 profile；私有 bundle 只在外层省略 handled 监听器，原事件发生 guards 在 cleanup 阶段检出漏事件，profile 分类拒绝。两次驱动都返回 1，核验器必须依据真实失败内容区分；不是仅凭退出码分类。原 Worker、驱动、生成 bundle 前后哈希不变，完整报告、私有副本和关键二进制/标准 bundle 输入归档。

这完成两个已知冲突的版本化精确映射及本地证据链。两个标准失败仍是 rejectionhandled.reason 与 MessagePort capture:true/default-profile 冲突，标准上游断言及未命中 D010 期待可见。document-scoped lifecycle HTML 未执行、三个既有 workerd 差异、其他 managed OS 状态/待写与跨 shard 排列、Windows sanitizer、完整 vendor TSan/fuzz/soak/native Mac/Hosted/T15–T17 和全新双构建逐字节复现仍未关闭。0.3.0 总体未完成，不作 T02/T04 等整项关闭；第 53 节及更早十三份封存保持原样。

## 第 53 轮验收（2026-10-17）：Managed 预热 Worker 接管失败回收

最新证据：`build-v030-dev/evidence/generation-adoption-cleanup-20261017/`；详细审校见[第 53 节](v0.3.0-glm-takeover-review.md#53-2026-10-17-managed-预热-worker-接管失败回收)。本轮修改四个项目文件，其中一个新增测试文件；Windows/Linux 各新增 9 个 CTest。Native `8fbb81e29abd102c62b5fb5f670b137945d7cdec`、Linux `7edd2380d97f593797e3d4b0c50e05b716bdf77d` 的 4342 个项目文件字节相同；根 HEAD 与既有修改保留，六份结果文档在验证后最后写入。

修复 `GenerationPool::create_adopted()` 的所有权回收遗漏：整个预热列表在调用入口转交所有权，但中途 `adopt()` 失败时，原实现只销毁失败位置并回收已接管的前缀，后续尚未访问的 Worker 会留下。现在失败位置及后缀全部销毁，前缀仍由原来的 executor 唯一回收线程负责。公开 C ABI v7、IPC EXIT 语义、Worker Runtime、43 个 overlay patch、SDK/reference 锁及 WPT expectation 均未改动。

两平台均先在原产品实现上运行九项真实进程用例：首个空位置失败后留下两个 Worker，中间位置失败后留下一个；末个位置失败、数量不匹配、零 Worker、身份不完整、策略无效、缺少 factory 和成功接管这七项对照通过。修复后同一份测试源码在两平台均通过；成功接管三个真实 Worker 后自然 drain，分别要求实际 OS exit 0、未强制终止、无 wait error，并验证不重建 Worker、回调只发生一次和重复 stop 的幂等性。

回收断言独立观察真实 OS 进程。POSIX 只有产品已经完成 reap 后的 ECHILD 才算成功，测试若自己捡到 zombie 会失败；Windows 在转交前保留独立进程句柄，查询退出状态，不再访问转交后可能已释放的 C API 指针。失败对照先记录存活进程，再由测试清理自己创建的子进程。失败回滚只证明回收，不能据此声称其 OS 退出码全部为 0。

最终源码私有产品对象对照 Windows 27 / Linux 27 项全部符合预期：只替换实际 `generation_pool.cc` 对象，真实测试对象、其余已记录对象/库和 Worker 不变；恢复旧逻辑会使首个/中间位置失败，省略四处校验分支的回收会使五类无效配置失败。每平台七份关键原始输入归档，其余输入由哈希绑定，隐式系统和工具链输入未全部逐文件归档。先前 native final1 的 27 项控制另行保留，不混作最终 54 项。

Windows strict Release 全量 **342：330 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **632：627 通过、2 个 WPT 文件失败、3 条件跳过**，均未排除测试。专项 Windows **86/86**、Linux **89/89**；GCC Debug TSan **86/86、零报告、零跳过**，100 个选中项目 C++ 编译输入实际插桩。新测试目标首次 Debug 链接缺少 SingleWorkerServer 实现，已按现有 executor 目标补齐该实现、Boost 依赖和一致的 GCC Beast 编译策略；首次构建失败日志保留，未计为 TSan 测试结果，也未运行该版的普通全量验收。最终两平台全量和 TSan 来自补齐依赖后的同一版源码。

固定 workerd 正式门禁 **22/22、零跳过**，在完整 Linux 全量日志归档后执行；同一普通 Linux 二进制 qualified root **3/3、零跳过**。正式门禁四后端的精确帧分别四连接/56 帧，Native 两模式也分别四连接/56 帧，SDK 两模式各 22 场景。WPT 保留 **84 份完整报告、14060 子测试**；完整失败清单：`worker_wpt_html_webappapis_scripting_processing_model_2_unhandled_promise_rejections_support_promise_rejection_events_js`、`worker_wpt_webmessaging_MessagePort_initial_disabled_any_js`。不修改 expectation 或 30 ms 容差，不用局部重跑替换全量失败。完整日志的既有 token/poison 诊断另存 `adoption-observations.json`，本轮九项接管用例没有这些诊断。

两个 WPT 失败已有第 26.4–26.5/27.3 节和 `docs/workerd-websocket-semantics.md` 的固定 reference 同例证据：默认 workerd 同样在 rejectionhandled 中省略 reason，并拒绝 MessagePort 用例的 capture:true；该用例因此没有进入原队列断言。第 27 节也已补强目标 handled 事件必须发生的检查。后续应沿用并复核这些证据，完成版本化 profile 的精确映射与标准 WPT 报告边界；不能把上游测试称为陈旧，也不能把 reason 改回与固定 reference 不同的值或放行整份文件的任意失败。

0.3.0 总体仍未完成。下一轮优先完成上述兼容性 profile 验收；其他未完成项包括 managed replacement/retire/quarantine/取消的实际 OS 状态覆盖、其他自定义驱动、本地 503 待完成内核写与池配置覆盖、跨 shard/owner exit/stop 排列、三类既有 workerd 差异与稳定时钟验证，以及 Windows sanitizer、完整 vendor TSan/fuzz/soak/native Mac/Hosted/T15–T17 和全新双构建逐字节复现。默认 vendor C、GCC 部分原子 fence 和 native Windows/macOS sanitizer 仍有覆盖边界。第 52 节及更早十二份封存证据保持原样，最终封存再次逐文件核验；本轮不作根仓库提交、推送或发布。

## 第 52 轮验收（2026-10-17）：Host drain 信号等待与真实退出状态

最新证据：`build-v030-dev/evidence/host-exit-status-20261017/`，审校见 [第 52 节](v0.3.0-glm-takeover-review.md#52-2026-10-17-host-drain-信号等待与真实退出状态)。本轮修改 15 个项目文件，新增 Windows 8 / Linux 9 个 CTest；Native `43b16a5848a3a22f4ced7728d6ea18bc2c57fefa`、Linux `32ce7158f16ee941c4be4f8c97049ed2f1c9279d` 的 4341 个项目文件字节相同，根 HEAD 与既有修改保留。

修复：启用进程信号的单 Worker Host 在 `complete_drain()` 中遗漏取消 Asio signal wait，导致 Worker 已退出并回收，`wait()` 仍不返回。两平台真实 RED 的正常、空闲、到期 drain 均超时，关闭信号的对照通过。最终在同一 owner loop 取消该等待，保留自然收尾；RED 到最终只改动该产品文件及 executor 目标的构建依赖，测试源码字节相同。

新增内部 C++ `WorkerExitStatus`，由原有唯一回收路径保存 OS 退出结果，经 executor、单 Worker 和静态池提供快照；C ABI v7、`EXIT.status=0` 的未使用字段语义、Worker Runtime 与 43 个 overlay patch 不变。未知结果不能视为成功，POSIX ECHILD 保持未知；Windows 保留真实 exit code，POSIX 区分正常退出与信号退出，另记 destroy 是否触发终止兜底。`destroyForced` 只记录 destroy 自身发出的兜底终止；Windows exit code 本身不能区分应用返回 1 和外部 TerminateProcess(1)，本轮结合定向场景与 token 日志判断。`wait()` 仍表示 Host 清理完成，需单独检查 Worker 状态。另修复 executor 测试在 Debug/TSan 下缺少 SingleWorkerServer 实现依赖的链接问题；没有移除该测试，首次全量中断日志完整保留，最终全量与 TSan 均来自补齐依赖后的相同项目字节。

Windows strict Release 全量 **333：321 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **623：618 通过、2 个 WPT 文件失败、3 条件跳过**。专项 Windows 76/76、Linux 78/78；GCC Debug TSan **77/77，零报告、零跳过**，98 个选中项目 C++ 编译输入实际插桩。默认 vendor C、GCC 对部分原子 fence 的支持和 native Windows/macOS sanitizer 仍有边界。

workerd 正式门禁 **22/22、零跳过**，在完整全量日志归档后运行；同普通 Linux 二进制 qualified root **3/3、零跳过**。WPT 保留 84 份完整报告/14060 子测试，失败仍按原结果记账，不修改 expectation 或 30 ms 时间容差。Windows SDK 两模式各 22 场景，精确帧两模式各四连接/56 帧；正式门禁四后端也分别验证四连接/56 帧。

原来两类 Host 退出诊断现已有明确对照：未关闭的流、忽略取消的长定时器及 drain 到期取消实际 exit 1；正常/空闲 drain、会清除定时器的合作取消和完成的单/双 Worker 写清理要求实际 exit 0。原有取消路径仍被执行；长定时器用于防止调度延迟把取消变成自然完成，没有增加延时隐藏诊断。最终全量保留 7 条 token 诊断记录，涉及 `host_admission_keepalive_streamed、host_admission_keepalive_streamed_drain_deadline、host_worker_deadline_independent、worker_exit_retained_timer`，并单列 poison 诊断；Native 不合作超时对照先进入 detached-resource poison，实际仍为 exit 1，不强求一定出现较晚的退出 token 日志。逐条见 `worker-exit-observations.json`；其他自定义驱动的 OS 状态仍需继续审校。

两平台私有控制只替换 `client.cc` 产品对象，保持真实驱动及其余已记录对象/库不变：丢弃状态和伪造成功均按预期被检出，Windows 27 / Linux 30 项控制结果全部符合预期。原始测试/Worker/Host 二进制及两个关键对象每平台归档九份；其他已记录输入按哈希绑定，隐式系统库未全部逐文件归档。

0.3.0 总体仍未完成。后续继续其他驱动/managed 生命周期的 OS 状态、本地 503 内核待写与池级覆盖、跨 shard 退出排列、三类已有 workerd 差异、WPT profile/陈旧 expectation/稳定时钟、Windows sanitizer、完整 vendor TSan/fuzz/soak/native Mac/Hosted/T15–T17 与全新双构建复现。原始构建准备失败及首次控制脚本编码错误保留于 `preparation-notes.md`，不计为产品验收。

> 当前进度（审校第 51 节）：Windows strict Release 全量 **325 项：313 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **614 项：609 通过、2 个 WPT 文件失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**，同普通 Linux 二进制 root **3/3、零跳过**；专项 Windows **66/66**、Linux **67/67**，GCC Debug TSan **66/66、零报告、零跳过**。 新增七项退出回归，修复 fetch 建立失败后的 client/abort listener 残留、退出前 GC 与 poison 返回值；原 42 patch 不变，新增 0043 并更新 overlay 锁。正常集成驱动明确检查实际 OS exit 0，Host wait/normalCleanup 仍不能证明每个 Worker 退出 0。WPT 2 个文件失败及其余终态/资源/平台矩阵仍开放，0.3.0 总体未完成。详见[审校第 51 节](v0.3.0-glm-takeover-review.md#51-2026-10-17-worker-真实退出与-fetch-建立失败清理)，交接入口 docs/v0.3.0-next-ai-handoff.md，证据 build-v030-dev/evidence/worker-exit-integrity-tdd-20261017/。

> 当前进度：修正写超时主用例的假通过，新增禁用配置和完整池写超时/drain/stop 四项；生产源码未改，两平台最终各三轮 21/21，两类产品错误副本均被对应断言检出。Windows 全量 **318：306/0/12**，Linux 全量 **607：602/2/3**（通过/失败/跳过）；workerd **22/22**、同二进制 root **3/3**、TSan **51/51、零报告、零跳过**。详见[审校第 50 节](v0.3.0-glm-takeover-review.md#50-2026-10-17-写超时测试假通过与完整池背压回归)。Runtime 退出诊断、本地 503 待完成内核写和其他矩阵仍开放，0.3.0 总体未完成；旧记录按各自快照理解。

> 当前进度：补齐本地 503 会话晚到写回调的三项真实 TCP 回归，以及完整双 Worker 池 stop/drain 的两项待读头回收；生产源码保持第 48 节原样，两平台各三轮 15/15，三类错误副本均被对应断言检出。Windows 全量 **314 项：302/0/12**，Linux 全量 **603 项：597/3/3**（通过/失败/跳过；含本轮 hr-time 失败）；workerd **22/22**、同二进制 root **3/3**、TSan 专项 **44/44、零报告、零跳过**。hr-time 固定 20 次诊断为 19 通过/1 失败，失败窗口与时钟跳变对应，全量失败未替换，稳定时钟环境复验仍待完成。详见[审校第 49 节](v0.3.0-glm-takeover-review.md#49-2026-10-17-本地-503-晚到写回调与完整池关闭回归)。完整池待完成写与其他资源/平台矩阵仍开放，0.3.0 总体未完成，以下旧记录按各自快照理解。

> 当前进度：共享外部接收入口在 begin_drain 返回后拒绝新提交，已排队 owner 回调也在 Session 创建前检查 drain。五个直接 API 场景两平台真实红测为三失败/两对照通过，相同红测对象的候选各三轮 15/15，三类错误副本被指定断言识别。Windows 全量 **309 项：297/0/12**，Linux 全量 **598 项：593/2/3**（通过/失败/跳过）；workerd **22/22**、同二进制 root **3/3**、TSan 专项 **39/39、零报告、零跳过**。新夹具使用两个直接 API Worker，完整池矩阵仍继续；详见[审校第 48 节](v0.3.0-glm-takeover-review.md#48-2026-10-17-drain-调用与已排队外部移交)。0.3.0 总体未完成，以下旧记录按其快照理解。

> 当前进度：static-pool 重选出的 Worker 在实际移交前再次退出时，原连接现在可继续重选健康分片；全部 Worker 退出则返回完整 503，已关闭门控的 stop/drain 结束待移交连接。新增五个完整池回归，原实现两平台各两失败/三对照通过，保留红测对象的候选各三轮 15/15，三类错误实现副本均被对应断言拒绝。Windows 严格 Release 全量 **304 项：292 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **593 项：588 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Linux Debug GCC TSan **34/34、零报告、零跳过**。详见[审校第 47 节](v0.3.0-glm-takeover-review.md#47-2026-10-17-重选-worker-后实际移交前的再次退出)；完整资源、兼容性与发布矩阵仍开放，0.3.0 总体未完成。

> 当前进度：全池 Worker 退出后，static-pool 的新 HTTP/HEAD/WebSocket 握手已在 Windows/Linux 返回完整 503；入口统一为池持有的 acceptor。两平台原产品各三个真实失败；保留红测对象的候选各 18/18，三类错误副本均被拒绝。Windows 严格 Release 全量 **299 项：287 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **588 项：583 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Linux Debug GCC TSan 扩大为 **29/29、零报告、零跳过**。详见[审校第 46 节](v0.3.0-glm-takeover-review.md#46-2026-10-17-全池-worker-退出后的前端-503)；完整选择器、资源与发布矩阵仍开放，0.3.0 总体未完成。

> 当前进度：第 43 节发现的 fetch-cancel 实际失败已定位到握手覆盖关闭 timeout，并由独立定时器修复；两平台真实红测及同对象候选成立。Windows 严格 Release 全量 **293 项：281 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **582 项：577 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Debug GCC TSan 生命周期/503/取消专项 **12/12、零报告、零跳过**。 原 Worker 测试两平台各连续 32 次通过。详见[审校第 44 节](v0.3.0-glm-takeover-review.md#44-2026-10-16-fetch-取消与待执行握手的交叠)；0.3.0 总体未完成，以下历史结果按各自快照理解。


> 当前进度：修复 adopt_socket 入口检查通过后、实际 post 前 owner 已结束导致的 TCP 滞留；三项真实进程/stop/drain 回归及同红测输入候选通过。Windows 严格 Release 全量 **292 项：279 通过、1 个 fetch-cancel 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **581 项：576 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Debug GCC TSan 生命周期/503 专项 **11/11、零报告、零跳过**。 详见[审校第 43 节](v0.3.0-glm-takeover-review.md#43-2026-10-16-移交入口检查至实际-post-的竞态)；证据 `build-v030-dev/evidence/adoption-submit-gate-tdd-20261016/`。0.3.0 总体未完成，以下历史记录按各自快照理解。


> 当前进度：修复 Worker 退出后首个/下一次 HTTP 读取连接未回收的问题；真实两平台红测与同字节候选通过，新增三个相邻状态回归。Windows 严格 Release 全量 **289 项：277 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **578 项：573 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Debug GCC TSan 生命周期/503 专项 **8/8、零报告、零跳过**。 详见[审校第 42 节](v0.3.0-glm-takeover-review.md#42-2026-10-16-worker-退出后的空闲-http-连接回收)；证据 `build-v030-dev/evidence/queued-adoption-exit-tdd-20261016/`。0.3.0 总体未完成，以下历史记录按各自快照理解。


> 当前进度：新增 pending accept 与真实 Worker 退出的三个集成场景，并修复 fetch-cancel 空闲接收超时夹具误报；临时缺陷副本反向验证有效，产品实现本轮无改动。Windows 严格 Release 全量 **286 项：274 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **575 项：570 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同一 Linux 二进制 root 补验 **3/3、零跳过**；Debug GCC TSan 生命周期专项 **4/4、零报告、零跳过**。 详见[审校第 41 节](v0.3.0-glm-takeover-review.md#41-2026-10-16-pending-accept-与真实-worker-退出回归)；证据 `build-v030-dev/evidence/pending-accept-exit-tdd-20261016/`。0.3.0 总体仍未完成；以下历史记录按各自快照理解。


> 当前进度：外部连接移交与正常停止的竞态已修复，TSan 有效红测 → 候选及正式项目回归均通过，见[审校第 40 节](v0.3.0-glm-takeover-review.md#40-2026-10-16-连接移交与停止的跨线程竞态修复)。Windows 严格 Release 全量 **283 项：271 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **572 项：567 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 门禁 **22/22、零跳过**，同一 Linux 二进制 root 补验 **3/3、零跳过**。 证据 `build-v030-dev/evidence/adopt-stop-race-tdd-20261016/`；0.3.0 总体仍未完成，以下旧记录按各自快照理解。


> 当前进度：原始帧 workerd 对照与门禁补强已完成，见[审校第 39 节](v0.3.0-glm-takeover-review.md#39-2026-10-16-固定-workerd-原始帧对照与门禁补强)。Windows 严格 Release 全量 **283 项：271 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **572 项：567 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 门禁扩为 **22/22、零跳过**，同一 Linux 二进制 root 补验 **3/3、零跳过**。 证据 `build-v030-dev/evidence/raw-frame-reference-tdd-20261016/`；0.3.0 总体仍未完成。


> 当前进度：接棒修复与验证已推进至[审校第 38 节](v0.3.0-glm-takeover-review.md#38-2026-10-16-接棒正常退出背压及精确帧边界)。Windows MSVC 严格 Release 全量 **282 项：270 通过、0 失败、12 条件跳过**；Linux 严格 Release/LTO 全量 **570 项：565 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 门禁 **18/18、零跳过**，随后同一 Linux 二进制以 root 补验 **3/3、零跳过**。 证据：`build-v030-dev/evidence/parent-stop-tdd-20261016/`；整套 0.3.0 尚未完成。以下早期记录按各自冻结源码理解。


The project reports its own contracts, adapted WPT, process integration, and environment-based sandbox verification separately. Passing any one layer cannot substitute for another.

## Testing Layers

1. C/C++ unit tests: protocol, header, policy, topology, audit, and structured parsing;
2. real worker contract: bundle, IPC, flow control, cancellation, timeout, sandbox, and fetch;
3. pinned WPT: each upstream file selected by the manifest runs in an independent worker realm;
4. framework differential: Node reference and real worker are compared vector-by-vector, with independent absolute assertions retained;
5. sanitizer/fuzz: ASan and UBSan project targets, planned Host TSan, and four libFuzzer harnesses;
6. benchmark contract: verify content, versions, and environment before allowing performance samples to be recorded.

Standard sources, WPT selection, and deviations are unified in [Standards and Conformance](conformance.md).

## Validity Rules

Long-running gates are fail-closed:

- when WPT is not configured, register the `wpt_conformance_not_configured` failure sentinel;
- checkout must equal the pinned commit in the manifest;
- the manifest and the actually registered paths are compared item by item, not just by count;
- the WPT realm must report a top-level unique positive integer `total` and `ranNothing: false`;
- expected failures and `notExecuted` are read only from the manifest and must reference a registered deviation;
- restricted binary audit must first prove that the symbol table, archive, and LTO marker are readable;
- Wasm C, JavaScript, and fixture resource constants must be consistent;
- overlay key, stamp, manifest, and configure dependencies must come from the same canonical input;
- differential vectors from the three frameworks must have independent absolute assertions, and the anchor count cannot be zero;
- negative controls must prove that gates catch bad input, not only positive tests where a correct implementation turns green.

Test validity is continuously demonstrated by the automated gates above; one-off audit reports drift with current code, so they are not retained as product-state documents.

## Common Commands

Basic build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

Pinned WPT:

```sh
cmake -S . -B build-wpt \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_WPT_ROOT=/absolute/path/to/pinned/wpt
cmake --build build-wpt
ctest --test-dir build-wpt --output-on-failure
```

The `HEAD` of the WPT root must equal the commit in `tests/wpt/manifest.json`.

The manifest also declares source augmentations. The rejection support source
keeps every upstream statement and adds callback-count cleanup assertions to two
named delayed-handling cases that could otherwise pass without observing
`rejectionhandled`. `wpt_rejection_presence_controls` verifies normal, missing,
unrelated, late and duplicate events, exact byte restoration, and retained reason
assertions. `wpt_rejection_presence_runtime_controls` demonstrates the old false
pass and the new specific failure using the real Worker. Neither control adds an
expected-failure waiver. Original default-workerd/standards disagreements remain
visible in the upstream-file CTests.

`wpt_adapter_contract_controls` also checks engines whose Error.stack contains
only frames: cleanup reports retain the original message as well as the stack.
An independent clock-window diagnostic may identify an unstable environment for
HR-Time, but its runs never replace a failed CTest or change upstream tolerances.


### Required workerd WebSocket comparison

Use the locked Node version (currently 24.14.0 with bundled undici 7.21.0)
on Linux x64. Install the exact reference asset and run the complete gate:

```sh
node tools/workerd-reference.mjs install /tmp/capsid-workerd/workerd
export CAPSID_WORKERD_REF=/tmp/capsid-workerd/workerd
python3.12 tools/setup-ws-interop-python.py /tmp/capsid-ws-clients
export CAPSID_WS_INTEROP_PYTHON=/tmp/capsid-ws-clients/bin/python
export CAPSID_WS_INTEROP_NODE_PACKAGE=/tmp/capsid-ws-clients/node-ws
node tools/run-workerd-ws-gate.mjs build build/workerd-ws-evidence
```

The installer checks the compressed asset digest, executable digest and version.
The required manifest contains 15 fixtures run on the reference and on single-worker,
static-pool and managed. The gate also requires outbound reports from all four
backends, comparator controls, API mutation controls and four independent-client
matrix runs and four two-Worker fanout runs: eighteen CTests in total. Missing reference,
missing registration, a skipped case, incomplete reports, mismatched source or
binary identities, absent observations and abnormal process shutdown fail the
gate. Comparison preserves each scenario's absolute assertions and permits only
the explicit deviations in [the semantics dossier](workerd-websocket-semantics.md).

The independent implementations are CPython 3.12.3 with websockets 15.0.1 and
Node 24.14.0 with ws 8.22.0, pinned in
`tests/fixtures/websocket-interop-lock.json`. The setup command verifies the
Python wheel and Node tarball hashes, installs the Python wheel in a virtual
environment and extracts the Node package without running install scripts.
Both implementations check runtime versions and loaded source digests before
opening sockets; optional Node native addons are disabled. CMake also accepts
`CAPSID_WS_INTEROP_PYTHON` and `CAPSID_WS_INTEROP_NODE_PACKAGE` cache paths.

The Python helper uses CPython's bundled Selector event loop on Windows. The
pinned 3.12.3 Proactor transport can raise from socket shutdown after a reset
before detaching the server, leaving peer teardown pending. This TCP/TLS-only
helper reads stdin in a thread and requires no asynchronous pipe or subprocess
support. Runtime versions, installed library source digests, protocol assertions,
callback diagnostics and peer exit requirements remain unchanged; Linux uses
its default loop.

The five `websocket_python_peer_*` CTests independently cover idle stop,
WS/WSS exchanges, certificate rejection and TLS resets, an interrupted session,
and a TCP reset after the Close exchange. Interrupted-session controls require
the peer to retain the failure and exit with a nonzero code after reporting its
terminal state. They cannot pass by killing the peer or suppressing callback
errors. The platform registration audit requires all five distinct scenarios
and rejects missing, aliased, disabled, inverted or skippable registrations.

Each backend executes 22 ordered cases: both clients exercise ws/wss echo,
client/server Close and certificate rejection; Python adds fragmented text and
binary with interleaved Ping/Pong. Node ws and Python each serve as the native
worker's outbound protocol peer for plaintext/TLS success and certificate
rejection. Every complete echoed payload has an independent SHA256 check,
including empty binary messages and a 180,000-byte pattern. A per-run challenge
binds live exchanges, and active sessions, SNI, both Close directions and normal
peer termination are checked. The inbound wss fixture terminates TLS in front of
the plaintext Host; outbound wss verifies the actual native client against each
library's TLS server. This does not certify a native TLS Host listener.

Compression is explicitly disabled with ws `perMessageDeflate:false` and Python
`compression=None`; the reference compatibility date/flags remain unchanged.
The earlier built-in Undici experiment remains supplemental evidence and is not
counted as the plan's Node ws implementation. Wrong bytes are rejected by live
client controls, while copied library sources with an unchanged version fail
identity checks. Missing Pair support now fails instead of returning 77.

The gate also validates the archived mutation report independently of the CTest
exit code. It requires the normal API baseline followed by the fourteen reviewed
faults, each with its intended assertion failure and no spawn error, signal or
timeout. Reports bind the gate run ID, client/reference settings, fixture set,
original and relocated API drivers, controller, validation module and Host/Worker
digests. The API-only baseline must retain `complete: false` for the full fixture
matrix while executing all 648 API assertions; its observations must match the
gate's normal API result. That API-only report now has reference parity; the full
inbound matrix still records the Close-code and send-budget differences. Every gate attempt replaces the current summary,
including a failed preflight, so a previous successful summary cannot represent
the latest failed attempt.
The [TDD follow-up](v0.3.0-glm-takeover-review.md#19-2026-10-15-测试补强与-tdd-修复)
closes the audit's directory-derived manifest, empty observations, whole-fixture
exceptions and missing outbound comparison gaps. Every fixture now returns
observed data. Variable Pong subsets and the finite send budget have explicit
contracts; other fields continue to compare. The outbound inventory distinguishes
35 reference scenarios (36 Capsid scenarios, including its dial deadline) from
178/179 invocations, including 141 bad-Accept attempts. Six EOF cases retain
callback, microtask and timer observations and check ignored post-disconnect
methods. EOF state and reason now compare directly without an exception.
Seven ownership invocations additionally hold the upstream handshake until the
HTTP body ends, observe actual TCP closure and absence of callbacks, and repeat a
stalled peer three times against a two-pending-dial budget. Positive controls
keep an already-open socket alive after HTTP completion and open another socket
while a streamed response body is still unfinished. Cross-request readyState
access must throw Error; only the two exact, verified engine diagnostic strings
are normalized after validation. The former pending-constructor lifetime
exception has been removed. Comparator samples retain all ownership repeats and
compress only the 140 repeated handshake failures.
The added `response.mjs` driver executes 32 constructor/body/clone outcomes and
real TCP Upgrade controls, including eighteen invalid endpoint couplings, two
simultaneous connections before and after them, delayed acceptance with buffered
DATA or DATA/Close, cross-request rejection, and outbound endpoint rejection.
It validates the actual client nonce, response headers, complete frame bytes and
peer EOF/reset before normalizing transport metadata. Client disposal cannot
count as peer termination. The former accepted-endpoint HTTP-500 exception has
been removed; both engines commit 101 before rejecting that coupling. Native
controls separately bound never-accepted coupling by the configured deadline and
verify cancellation ACKs, callback suppression and two-slot reuse. These deadline
controls check Capsid's resource policy; they do not establish an identical
never-accept timeout in workerd.
Each run saves its command, registration, JUnit, full CTest log and eight reports
in a fresh directory. `summary.json` identifies the successful comparison.
The Linux Release job in `testing-validity.yml` installs and requires this gate.

`workerd_ws_gate_controls` checks both accepted reports and rejected inputs.
`workerd_ws_mutation_controls` first requires a passing real API baseline, then
requires incorrect wire close codes/reasons, duplicate dictionary reads and a
string error payload to fail at their specific assertions. It runs isolated
fixture copies and never modifies the production fixture pack.
The release workflow calls the same commit's validity workflow before packaging;
package checkout uses the event SHA. Local workflow audits verify this dependency;
they do not substitute for a hosted release run.
`host_session_tracking_single-worker`, `host_session_tracking_static-pool` and
`host_session_tracking_managed` keep multiple HTTP connections open while
removing a non-final connection and creating another, then require normal Host
shutdown. These cases detect lost session registrations that a forced teardown
would hide. `host_generation_pool_contract` pauses actual notification and
replacement paths while releasing the last external pool owner; the real
`websocket_managed_deploy-persist-failure` test retains the end-to-end rollback
and resource-reuse assertions.

### Egress and TLS Targeted Regression

Domain egress rules authorize the addresses actually resolved for that domain, including private, loopback, link-local, and other protected ranges; explicit IP/CIDR deny still takes precedence. That authorization belongs only to requests initiated by domain name and does not turn the same address into a directly requestable IP literal. Policy unit tests and real worker regressions are covered by `egress_policy`, `worker_fetch_hostname_authorizes_resolved_loopback`, `worker_fetch_host_deny_diagnostic`, `worker_fetch_protected_deny_diagnostic`, `worker_fetch_explicit_deny_diagnostic`, and `worker_direct_fetch_http_matrix`. The final authorization semantics and the three diagnostic text types are described in [Host capability policy](capability-policy.md).

The pinned mbedTLS version once exhibited inconsistent signature-algorithm checks under a mixed TLS 1.2/1.3 client configuration: `mbedtls_ssl_get_pk_type_and_md_alg_from_sig_alg()` can parse the TLS signature scheme `0x0804` (`rsa_pss_rsae_sha256`), but the TLS 1.2 legacy hash/signature byte-pair check treats the high byte `0x08` as an unknown hash and rejects ServerKeyExchange. The client has already declared the scheme in ClientHello, and a TLS 1.2 server selecting it is legal behavior, so this is not a server or application configuration problem.

`patches/txiki/0009-lws-vendor.patch` only makes the TLS 1.2 client's ServerKeyExchange failure branch compatible with `rsa_pss_rsae_sha256/384/512`: each branch remains constrained by the corresponding hash and PKCS#1 v2.1 compilation capabilities, and the later "server-selected scheme must have actually been offered by the client" check is unchanged. The fix does not relax generic TLS 1.2 helpers, nor does it avoid negotiation by forcing TLS 1.2-only.

`worker_direct_fetch_https_tls12_rsa_pss` starts a local OpenSSL `s_server`, forces `-tls1_2 -sigalgs rsa_pss_rsae_sha256`, and then lets a real worker complete a trusted HTTPS request. On Linux it additionally registers `worker_strict_sandbox_https_tls12_rsa_pss`, which uses the same handshake regression to cover the `--strict` sandbox and requires the worker to exit cleanly; it is not registered on macOS, and like other strict-sandbox gates it is excluded from the ASan/TSan matrix. Targeted reproduction command:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(egress_policy|worker_fetch_.*diagnostic|worker_fetch_hostname_authorizes_resolved_loopback|worker_direct_fetch_http_matrix|worker_direct_fetch_https_tls12_rsa_pss|worker_strict_sandbox_https_tls12_rsa_pss)$'
```

ASan example:

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCAPSID_ENABLE_ASAN=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure \
  -E '^(worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca|worker_strict_sandbox_https_tls12_rsa_pss)$'
```

UBSan replaces the switch with `CAPSID_ENABLE_UBSAN=ON`. Fuzz builds use Clang, `CAPSID_BUILD_WORKER=OFF`, and `CAPSID_BUILD_FUZZERS=ON`.

### TSan (M1C gate)

`CAPSID_ENABLE_TSAN` is configured: a dedicated Linux **GCC** Debug build that is not shared with ASan/UBSan (CMake configuration rejects the combination). It must pass before M1C acceptance; it is a mandatory gate before M2 multi-worker work begins.

TSan uses a dedicated Linux/GCC Debug build. Do not mix with ASan, UBSan, LTO, fuzz, or benchmark runs. **The supported TSan compiler in this environment is GCC**: Alpine/musl clang does not ship a TSan runtime (`libclang_rt.tsan_cxx.a` does not exist), so configuration rejects it and requires switching to GCC (`-DCMAKE_CXX_COMPILER=g++`). The first batch must cover at least command/event handoff between the HTTP event loop and worker threads, concurrent keep-alive, disconnect/cancel, timeout, and shutdown/reap. Any report from first-party code fails; third-party suppressions must be limited to specific external symbols, explain why, and cannot use broad rules to hide Host, Runtime, or IPC code. TSan results only prove race detection and are not QPS, latency, or CPU conclusions.

**Known coverage gap (compiler-diagnostic degradation, not race suppression)**: GCC 15.x's libstdc++ reports `-Werror=tsan` for `std::atomic_thread_fence` under `-fsanitize=thread` (Boost.Asio's fenced block uses that primitive). The build only downgrades that warning class to non-fatal (`-Wno-error=tsan`, inherited through the `capsid_sanitizers` INTERFACE); TSan instrumentation itself stays on—a probe with an injected race is still detected. The cost is that **TSan does not instrument `std::atomic_thread_fence` itself**, so races near fences may be missed; this is a documented diagnostic degradation and does not affect detection for all other synchronization primitives.

TSan has hard environment requirements: Clang TSan initialization must call `personality(ADDR_NO_RANDOMIZE)` to disable ASLR, so containers under the default Docker/containerd seccomp profile (including local bench/build containers) fail directly with a CHECK at `tsan_platform_linux.cpp:282`—this is not a code defect but an unmet environment prerequisite. The TSan gate must run on a real VM (GitHub hosted runner) or in a container with `--security-opt seccomp=unconfined`. In addition, TSan worker shadow memory mapping requires large contiguous virtual address space; sanitizer builds (`CAPSID_ASAN_BUILD` / `CAPSID_TSAN_BUILD`) disable the production RLIMIT_AS while keeping the QuickJS heap cap.

The CI `sanitizers` matrix adds a dedicated `tsan` entry: Clang compilation, `CAPSID_BUILD_HOST=ON`, and inside the job builds OpenSSL 3.5 from pinned source (SHA-256 verified) into `/opt/openssl35` (ubuntu-24.04 ships 3.0, which does not satisfy Host's 3.5 contract). The asan/ubsan entries keep their original configuration.

**The metrics-enabled path is the other half of the TSan gate**: M1C acceptance A/B evidence is generated with `CAPSID_HOST_IPC_METRICS=1`, and metrics are written by worker threads, read by the IO thread, and reset as a whole (`write_metrics_line` does `metrics_ = Metrics{}`); the lock-free cross-thread access is a real race (frozen RED: `host_single_worker_integration_metrics`). TSan passing with metrics off is not enough—the evidence path must also be race-free. `host_single_worker_integration_metrics` (ctest entry, `CAPSID_HOST_IPC_METRICS=1` environment) is an M1C gate alongside the ordinary integration tests.

The fanout matrix uses actual application instance and connection UUIDs. The
reference has two Worker services, single-worker mode uses two independent
one-Worker Hosts, and static-pool/managed use one Host with two Workers. Discovery
is bounded to 64 real connections and records every identity and clean Close;
Linux reuse-port distribution is not assumed to be round-robin. Two connections
per observed Worker exercise send/readyState/close ownership in 24 probes, plus
six probes while the target handler is demonstrably pending. The fixture pack's
original one-Worker fanout case also requires actual identities. The gate
validates raw memberships, exact reference Error diagnostics, cleanup and
liveness before normalizing UUIDs. Its controls run real Host/Worker processes
and preserve baseline/mutant reports under the gate archive. These tests do not
claim all fanout workloads, worker replacement or production platform coverage.

## Platform Contract Gates

Platform gates separately prove "native development" and "production isolation"; they cannot replace each other:

- Linux Release is the v1 production gate and must verify strict sandbox, cgroup, and required READY feature bits in a delegated environment;
- macOS native-dev runs platform-neutral Host unit tests and real single-worker loopback integration; strict sandbox requests must fail as negative controls;
- Windows native-dev uses the `windows-latest` hosted runner: MSVC + Ninja builds Runtime, worker, bytecode compiler, and Host, runs the platform-neutral matrix, and actually starts Host and worker on the Windows host, covering source/trusted-bytecode identity, `capsid:env`, request, streaming, cancel, timeout, crash/reap, and loopback-only negative controls;
- Windows cross-compilation, Wine, WSL2, or a Linux container cannot substitute for hosted native Windows execution evidence;
- Windows/macOS native-dev passes must not be written as production sandbox passes; conversely, the Linux production gate cannot replace Windows development usability.

Windows supports single-worker and multi-shard static-pool: multi-shard scenarios run through a pool-level shared acceptor instead of `SO_REUSEPORT`. The managed Host remains unavailable, `capsid:fs` is degraded but usable, and strict sandbox is not implemented on Windows; see [Platform support overview](platform-support.md). Implementations must not turn green by skipping worker tests, disabling trusted bytecode, or replacing the listener with a non-native Linux VM.

## Environment-Based Sandbox Evidence

On Linux, Binding filesystem probes and strict TLS trust-file tests require a
filesystem supported by Landlock. For a workspace on a WSL Windows mount, select
an explicit native Linux directory with
`-DCAPSID_SANDBOX_TEST_DATA_DIR=/tmp/capsid-test-data-<unique-build-id>`.
The tests keep their original allow/deny and trusted/untrusted assertions; the CA
fixture is copied byte-for-byte. Record this directory and the filesystem when
reporting results. Moving the fixture does not establish support for the mounted
filesystem itself.

Active-state directory, size, and foreign-owner checks are separate CTests. Only
the foreign-owner case can skip for unavailable ownership-change permission;
it keeps a valid persisted document and verifies successful recovery before
changing its owner. Both strict and ordinary process environment tests seed
host canaries and require an empty child process environment.

The package setup test runs serially because it may rebuild the current binaries.
Package smoke requires its successful CTest fixture setup; an ordering-only
`DEPENDS` property must not allow a stale archive to stand in for a failed build.
`worker_package_metadata_controls` uses the same successful package fixture,
then requires the normal archive to pass before injecting SBOM faults. The
smoke and reproducibility drivers check declared component identities against
source locks, complete file inventories, actual checksums and relationships.
`package_reproducibility_controls` covers missing/duplicate header identities,
manifest omissions and corruption, canonical record changes, and actual binary
difference records. A successful reproducibility comparison permits recorded
binary differences under the current contract; it does not establish byte-for-byte
reproducibility or a complete transitive dependency/license audit.


When a normal host lacks cgroup delegation or namespace permissions, the corresponding test returns CTest skip code 77. That only means the environment lacks the prerequisite; it cannot be recorded as a pass.

`.github/workflows/testing-validity.yml` contains five independent job classes:

- Ubuntu Release/LTO, pinned WPT, benchmark smoke, and privileged delegated sandbox, and generates the txiki.js upgrade report;
- Ubuntu ASan, UBSan, and TSan regular matrix; the TSan entry uses Clang and builds Host (including OpenSSL 3.5 built from local source); ASan excludes only two strict-sandbox network/TLS exit items already covered redundantly by the Release strict gate and the ASan non-strict same-function gate, because seccomp kills the process during instrumented runtime teardown;
- Clang/libFuzzer's four bounded corpus gates;
- macOS 14 POSIX host-library and non-worker unit matrix;
- `windows-latest` MSVC host-library, builds Runtime/worker/Host and runs platform-neutral tests; Linux-only tests are absent under the "not registered means skipped" or `SKIP_RETURN_CODE 77` principle.

All five job classes produce hosted evidence, uniformly gated by the final `hosted-evidence-index`; any job failure fails the whole run.

JUnit, build metadata, and upgrade reports are saved as workflow artifacts; generated report copies are not committed to the repository. The final `hosted-evidence-index` job downloads each job's evidence, writes the run URL, commit SHA, each evidence file's SHA-256, and an evidence tree summary, and requires all dependent jobs to succeed. The delegated sandbox script treats 77 as CI failure; ordinary runner environment skips cannot substitute for it.

All third-party actions are pinned to reviewed 40-character commit SHAs. The in-repo `testing_validity_workflow_audit` also locks the action list, four job classes, security gates, and CTest JUnit relative paths; `--test-dir` already determines the output root, and writing the build directory again into `--output-junit` is forbidden so reports and artifacts do not read a nonexistent two-level path.

## How Current Evidence Is Obtained

Test counts and results are based on the current build tree:

```sh
ctest --test-dir build --show-only=json-v1
ctest --test-dir build --output-on-failure
```

The pinned WPT file set is determined by `tests/wpt/manifest.json`; delegated sandbox positive tests must pass in a separate privileged environment and cannot be replaced by a normal host skip. CI generates a txiki.js upgrade report and hosted evidence index for each commit; those commit-bound artifacts with digests are the evidence for that run, and this document does not copy one day's counts.

WPT source integrity checks compare selected files and their declared resources
against the pinned Git blobs during build and before the worker cases. Preserve
`Testing/Temporary/LastTest.log`: successful WPT cases print their full
`WPT_REPORT`, including accepted assertion failures and original diagnostics.
The report validator, harness comparison and CLI entry controls are part of
CTest. CLI controls run both real paths and directory aliases so a skipped
module entrypoint cannot count as a successful audit.

WebSocket aggregate capacity is covered by `host_channel_budget` (shared ancestors,
in-flight buffer leases, metrics, and concurrent reservations),
`host_channel_generation_lifecycle` (shared service quota and replacement), and
`host_dns_dialer_lifecycle` (shared pending-dial quota). The independent TCP/HTTP
`websocket_budget_single-worker`, `websocket_budget_static-pool`, and
`websocket_budget_managed` cases each exercise service/listener channel ceilings,
pending dials, service/listener network queue refusals, HTTP health, capacity reuse,
live metrics, and clean shutdown. The static-pool case runs three shards.

```sh
ctest --test-dir build --output-on-failure \
  -R '^(host_channel_budget|host_channel_generation_lifecycle|host_dns_dialer_lifecycle|websocket_budget_(single-worker|static-pool|managed))$'
```

The raw Node peer uses the client version pinned in
`tests/fixtures/workerd-ws/reference-lock.json`. Its full JSON stdout records test
and helper hashes, binary hashes, observed budgets, assertions, and shutdowns.
When collecting successful cases, retain `Testing/Temporary/LastTest.log` because
CTest's default JUnit success-output limit can truncate these reports.

## 2026-10-16 本地 GCC TSan 生命周期专项

本机 WSL 的默认 GCC TSan 最小探针出现 `unexpected memory mapping`，在进程范围运行 `setarch x86_64 -R` 后通过。
这只改变调试命令及其子进程的 personality；构建目录必须独立使用 Debug/GCC、`CAPSID_ENABLE_TSAN=ON`、LTO/ASan/UBSan OFF，
并启用 Host、Worker 和测试。先构建同一配置的 `test-host-single-worker-lifecycle` 与 `capsid-worker`，兼容性 ID 检查保持开启。

```sh
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=0:exitcode=66 \
  ctest --test-dir <独立的-TSan-build> --parallel 1 --no-tests=error \
  --verbose --output-on-failure -R '^host_single_worker_controllable_lifecycle$'
```

`halt_on_error=0` 使回归在报告后继续完成进程清理，`exitcode=66` 仍使任何 race 报告失败，不添加 suppression。
本轮永久项目用例 1/1、零报告；这里只证明该生命周期专项。完整 TSan 门禁与其他 sanitizer/Hosted 验收仍按上文要求执行。
详细红绿记录见[审校第 40 节](v0.3.0-glm-takeover-review.md#40-2026-10-16-连接移交与停止的跨线程竞态修复)。

## 2026-10-16 external-accept 与 Worker 退出专项更新

第 41 节的最终 TSan 选择范围为四个实际 CTest，替代前一节仅执行单个生命周期用例的专项范围：

```sh
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=0:exitcode=66 \
  ctest --test-dir <独立的-TSan-build> --parallel 1 --no-tests=error \
  --verbose --output-on-failure \
  -R '^(host_external_accept_exit_|host_single_worker_controllable_lifecycle$)'
```

同配置构建 `test-host-single-worker-lifecycle` 和 `capsid-worker`，并配置固定 Node 可执行文件。
此专项 4/4、零报告；没有据此宣称完整 TSan 矩阵完成。三个退出场景的正常终态必须包含实际 Worker PID 回收。
普通构建另执行 `worker_fetch_cancel_lifecycle`，其一个 CTest 内含立即取消和跨过一次真实接收空闲超时后的取消。
两轮都要求实际 TCP 关闭和 request ID 复用；30 秒总预算匹配两个独立 Worker 场景，原有每阶段 deadline 未放宽。
详细原始失败、缺陷副本和最终全量见审校第 41 节及同名证据目录。


## 2026-10-16 Worker 退出后的连接回收专项

第 42 节将 external-accept 场景扩为六项，连同原生命周期及完整 503 场景，下面正则现选择八个实际 CTest，前一节四项范围为历史记录：

```sh
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=0:exitcode=66 \
  ctest --test-dir <独立的-TSan-build> --parallel 1 --no-tests=error \
  --verbose --output-on-failure \
  -R '^(host_external_accept_exit_|host_single_worker_controllable_lifecycle$|host_admission_worker_death_returns_503$)'
```

同配置构建 test-host-single-worker-lifecycle、test-host-admission、capsid-worker，并配置固定 Node。当前专项 8/8、零报告，不等于完整 TSan 矩阵。
Linux 普通构建连同 worker_fetch_cancel_lifecycle 的针对性回归为九项，Windows 不注册 Linux 专属 503 注入，针对性八项；fetch-cancel 仍包含立即取消和真实空闲超时后取消两轮。
排队移交、已接受空闲连接、完成响应后 keep-alive 均须观察实际 TCP 关闭和 PID 回收，不能只验证返回值。


## 2026-10-16 adoption 提交与关闭的专项

第 43 节新增 pre-post-worker-exit、pre-post-stop、pre-post-drain；沿用上一节正则现在实际选择 11 项：

```sh
setarch x86_64 -R env TSAN_OPTIONS=halt_on_error=0:exitcode=66 \
  ctest --test-dir <独立的-TSan-build> --parallel 1 --no-tests=error \
  --verbose --output-on-failure \
  -R '^(host_external_accept_exit_|host_single_worker_controllable_lifecycle$|host_admission_worker_death_returns_503$)'
```

同配置构建 test-host-single-worker-lifecycle、test-host-admission、capsid-worker，并配置固定 Node。11/11、零报告只适用于这一专项。
普通构建另加 worker_fetch_cancel_lifecycle，Linux 针对性 12/12；Windows 没有 Linux 专属 503 注入，针对性 11/11。
Windows 随后完整 292 项中 fetch-cancel immediate 失败；32 次原程序独立诊断又复现 1 次。针对性通过不能代替全量结果，原始失败与独立跟踪均保留。
分配屏障只在测试程序中启用，须校准实际 socket 分配、确认 Worker 仍可用、owner 已 join 且 io_context.stopped 后才恢复真实移交。
不能用入口先拒绝、销毁 facade 后才关闭连接、强杀 fixture 或只检查返回值代替 TCP/PID/健康 HTTP challenge 的实际断言。


## 2026-10-16 pending-handshake fetch 取消

`tjs_fetch_abort_pending_handshake` 是真实 native HttpClient 与本地 TCP 的受控单测，直接运行 test-tjs-fetch-abort，无 Worker 参数；不能替换为打印成功的脚本或允许跳过。
原 `worker_fetch_cancel_lifecycle` 源码和 30 秒外部超时保持不变，覆盖真实 Worker immediate/receive-timeout 后的连接关闭及 id 复用。
普通定向矩阵在第 43 节正则中加入 `|tjs_fetch_abort_pending_handshake$`，Windows 12/12、Linux 13/13；TSan 九项移交退出 + 原生命周期 + 严格 503 + 新取消单测为 12/12。
本轮所有原始红测、失败构建和探针仍保留；新定时器的验证不能只取连续通过次数，须同时核对原产品真实失败、同测试对象候选及实际 TCP 终态。
普通 TSan 配置未插桩 vendor httpclient.c；本轮额外显式插桩实际 C 产品对象并运行同一 native 回归，1/1 通过、零报告，__tsan_read*/write* 符号已核对。完整 vendor/依赖插桩仍需另行验收。


## 2026-10-16 完整池 pending accept 故障分派

新增 host_static_pool_fault_first-worker-exit、host_static_pool_fault_last-worker-exit、host_static_pool_fault_first-worker-exit-websocket，三平台均必须注册，不允许 skip/invert/disable。
配置固定 CAPSID_HOST_TEST_NODE 与 CAPSID_WS_INTEROP_NODE_PACKAGE；真实池 fixture 为 test-host-static-pool-faults，驱动为 test_host_static_pool_faults.mjs。
先观察两个真实 Worker 的身份和 OS 创建顺序，等待实际退出容量及 PID 回收，再发第一个 TCP 请求；第三项必须直接以 WebSocket 作为首个连接。
运行普通专项时将第 44 节选择器加入 host_static_pool_fault_，Windows 15/15、Linux 16/16；TSan 同配置构建新 fixture 后沿用专项正则加入该前缀，Linux 15/15。
每场景必须检验动态 HTTP challenge、四客户端 text/binary 精确回显、正常 Close、fixture 自行 stop/wait、所有初始 Worker PID 回收和实际端口关闭。
Linux TSan 不执行 Windows IOCP release/assign 分支；native 旧实现两项失败、同红测对象候选 9/9、仅 move 的错误副本两项失败是该分支的普通构建证据。
前期 Blob/SDK 路径/错误 PID 选择失败与修正全部保留；没有正确注入指定 Worker 的运行不能作为故障覆盖证明。全池无 READY 的 503 和更晚移交/关闭阶段继续待补。

## 2026-10-17 外部移交的 drain 窗口专项：审校第 48 节

Windows strict Release 全量 **309 项：297 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **598 项：593 通过、2 个既有 WPT 失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同普通 Linux 二进制 root **3/3、零跳过**；GCC Debug TSan 专项 **39/39、零报告、零跳过**。

SingleWorkerServer 最终接收门控和已排队 owner 回调新增 draining 检查，false 保留原 handle/executor。新增 host_external_drain_ 五场景：drain 后提交、实际准备途中 drain、先排队再 drain、owner 已处理 drain 后提交、不 drain 的正常对照。
原实现两平台前三项实际返回 HTTP 200，后两项通过；仅替换一个产品对象、保留 RED 测试对象/驱动/头文件/库/Worker，各三轮 15/15。漏提交门控、漏 owner 检查、消费拒绝连接分别被 2/1/3 项断言识别，夹具均正常退出、无强制收尾。
每场景已有 WS 共 24 帧/393978 payload 字节、1000/done 关闭，独立 Worker 两次动态 HTTP challenge/同 instance，成功 drain 自然 wait/finished=true/forced=0，两 PID 与两 listener 均回收。
新夹具直接使用两个 SingleWorkerServer API Worker，没有启动完整池；原完整池五项移交和六项故障回归仍在专项/全量/TSan 中实际执行。新范围不能代替完整池跨 shard 和本地 503 write completion 矩阵。

专项选择器在第 47 节范围中加入 host_external_drain_，普通 Windows 39/39、Linux 40/40；TSan 同配置先构建 test-host-external-drain 和原选中夹具/Worker，再以原范围加此前缀运行 39 项。14 条实际编译命令中 13 个项目 C++ 输入插桩，新夹具对象核对 __tsan_read*/write*；默认 vendor HTTP C 和更广依赖未完整插桩。
注册真实性控制 256/256，必需项 Linux 81/Windows 72/Darwin 73；五项不得删除、换场景/驱动/fixture/Worker/Node，或 skip/invert/disable/移除超时。Darwin 无实机验收，Windows IOCP 无 sanitizer 验收。
Native SDK 两模式各 22 场景、workerd 四后端精确帧各 4 连接/56 帧；WPT 84 报告/14060 子测试、20 expected/3 unexpected/1 missing expected；既有两个文件失败和 12/3 条件跳过没有隐藏，锁与 expectation 未动。

证据 build-v030-dev/evidence/external-drain-admission-tdd-20261017/；Native 0a796c0413b6aff3ab7ae2df4832947e0e21daf4、Linux 34f69f8c402ac3a50414737ebe586cc9094f3494 的 4333 个项目文件字节一致。根目录仅六份晚写结果文档，旧八份封存逐文件复核未变、历史另一个 build 的 49 进程保留。读 [详细审校](v0.3.0-glm-takeover-review.md#48-2026-10-17-drain-调用与已排队外部移交) 和最新交接入口；完整观察从 LastTest 对应段提取，不依赖截断的 passed JUnit stdout。
完整池跨 shard/成功 post 后 owner exit/stop 的其他排列、本地 503 写完成与 stop/drain、其他取消与资源矩阵、三类既有 workerd 差异/WPT profile、完整 sanitizer/fuzz/soak/native Mac/Hosted/T15–T17/全新双构建继续开放。T02/T04/T06–T08/T10–T17 和 0.3.0 总体未整体验收，未提交、推送或发布。

## 2026-10-17 本地 503 晚到回调与池关闭专项：审校第 49 节

Windows strict Release 全量 **314 项：302 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **603 项：597 通过、3 个 WPT 文件失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同普通 Linux 二进制 root **3/3、零跳过**；GCC Debug TSan 专项 **44/44、零报告、零跳过**。

本轮仅六个测试/注册文件变化，生产源码未改、没有新产品 RED。新增三个真实私有 503 会话场景和两个完整双 Worker 池 stop/drain 场景，两平台最终输入各三轮 15/15。
会话测试直接 include 原 .cc，单步 owner、独立 client io_context 固定实际响应字节先于写回调；GET/HEAD 的重复 close、注册表释放、队列共享持有、一次结束、实际回调排空及对象回收都有断言，另有正常 GET 对照。
完整池验证两个动态 HTTP instance、实际 WS 八帧与 Worker 退出后 1006/PID 回收、本地 GET/HEAD 503 和 preflight 204/403；三个不完整请求头经第四份完整响应建立已接收屏障，重复 stop/drain 三次，TCP 两秒内零字节结束、两名 wait 均完成、端口释放。Worker 在 drain 前已退出，aggregate finished=false/forced=0，不冒充 finished=true。
去掉写共享持有、破坏一次结束、遗漏 unavailable 清理的错误副本分别触发两项指定失败，其余三项对照通过。unit 混合 TU 以相同测试源重编译产品 include；完整池仅替换产品对象，保留 fixture 对象、SingleWorker 对象、库、Worker 和驱动。完整池错误副本正常退出、无强制清理。
两个 target 沿用相邻夹具的 GNU -Wno-maybe-uninitialized，另在测试 include 处豁免 GCC subobject-linkage 一条诊断；其余 strict 检查和运行断言启用，TSan 无 suppression。初始 synthetic 路径/GCC 准备失败归档，不计产品 RED。

新增选择前缀 host_unavailable_lifecycle_、host_static_pool_local_shutdown_；普通专项 Windows 44/44、Linux 45/45，TSan 构建两个新 target 和原选中夹具/Worker 后执行 44 项。18 条实际编译命令中 17 个项目 C++ 输入插桩，私有产品随 unit TU 插桩并核对对象 __tsan_read*/write*。注册控制 288/288，必需项 Linux 86/Windows 77/Darwin 78，新增五项不能删除、换命令/场景或 disable/invert/skip/取消有限超时。
固定 workerd/SDK 锁不变；Native SDK 两模式各 22 场景，四后端精确帧各 4 连接/56 帧。WPT 共 84 报告/14060 子测试、20 expected/4 unexpected/1 missing expected，其中 81 份接受、3 份拒绝；两项既有文件失败、本轮 hr-time 文件失败和 12/3 条件跳过完整保留。
hr-time/basic.any.js 在本轮全量新增失败，Date/performance 增量差 1433.497 ms。初始编排因新增失败中止并保留退出记录，后续独立执行 root/正式门禁。未改原 30 ms 容差、WPT expectation 或产品时钟，也没有以重跑覆盖全量。固定 20 次诊断保留 19 通过/1 失败；失败进程内增量差 1325.998 ms 对应 REALTIME/MONOTONIC 跳变 1325.120 ms，采样读取跨度 0.960 µs，支持环境时钟跳变的判断。全量只有分钟级用例时间，不能把它当作同等精度的进程内定位；Linux 全量仍是 3 失败，稳定时钟环境复验未完成。
证据 build-v030-dev/evidence/local-unavailable-lifecycle-tdd-20261017/；Native e5ee2016583a9c5826a3288ad2f17d908f333b33、Linux c5686af18d70c61cd7c37e757c1daf0ac3e10e66 的 4336 个项目文件字节一致，冻结后仅晚写六份结果文档。旧九份封存保持原样，另一历史 build 的 49 个进程保留。[详细审校](v0.3.0-glm-takeover-review.md#49-2026-10-17-本地-503-晚到写回调与完整池关闭回归)与最新交接入口为后续依据。
完整池真实待完成写/写超时及其更多并发排列、其他取消和资源、既有 workerd/WPT 差异、hr-time 稳定时钟环境复验、完整 sanitizer/fuzz/soak/native Mac/Hosted/T15–T17/全新双构建继续开放。T02/T04/T06–T08/T10–T17 与 0.3.0 总体未整体验收，原仓库未提交或发布。

## 2026-10-17 写超时真实性和完整池背压：审校第 50 节

Windows strict Release 全量 **318 项：306 通过、0 失败、12 条件跳过**；Linux strict Release/LTO 全量 **607 项：602 通过、2 个 WPT 文件失败、3 条件跳过**。固定 workerd 正式门禁 **22/22、零跳过**；同普通 Linux 二进制 root **3/3、零跳过**；GCC Debug TSan 专项 **51/51、零报告、零跳过**。
原 host_write_timeout_cancels 会立即读取大响应，仅凭很快 EOF 不能证明超时；两平台只移除真实产品的写定时器、保持原 fixture 等输入，旧三项仍全过，确认主用例漏检。不能据此判断人为作弊。本轮重写该 fixture，保留原三个场景并新增禁用写超时、完整双 Worker 池写超时/drain/stop 四项，生产源码不改。
真实 8 MiB 正文、小接收窗口在 connect 前设置、MSG_PEEK 不消费头/起始正文、每 shard 429 容量屏障；所有 Worker 的动态 200 恢复先于正文读取，之后要求原响应截断。禁用控制等待 800 ms 仍为 429，恢复读取后收全正文。完整池要求两份不同实际 instance，drain 正常 wait/finished=true/forced=0，成功路径不借 request_stop。
最终两平台各三轮 21/21。空写定时器副本指定三项失败/四项控制通过；错误启用 0 配置副本一项失败/六项控制通过。仅替换 SingleWorker 产品对象，新 fixture/池对象/库/Worker 保持同输入；旧对象只在首次审计期间固定，保留旧源/副本二进制/日志/命令/摘要。最终各归档三个原对象和正常测试二进制。
普通专项 Windows 51/51、Linux 52/52；TSan 51/51、零报告，核对 21 条编译命令、20 个项目 C++ 输入插桩，默认 vendor HTTP C 未插桩；注册控制 365/365，必需项 Linux 93/Windows 84/Darwin 85。strict 编译和编排准备错误不计产品 RED。
固定 workerd/SDK/reference 锁、ABI v7、42 patch 与 WPT expectation 未动；四后端精确帧各 4 连接/56 帧，Native SDK 两模式各 22 场景。WPT 共 84 份完整输出报告/14060 子测试、20 expected/3 unexpected/1 missing expected；82 份被验证器接受，2 份失败证据被拒绝。promise-rejection-events 和 MessagePort_initial_disabled 两项既有文件失败保留，Windows 12/Linux 3 条件跳过未隐藏。
本轮 hr-time/basic.any.js 通过。第 49 节的完整 3 失败及 20 次时钟诊断保留在原封存中，本轮结果不覆盖它们；没有改原 30 ms 容差或 WPT expectation，稳定时钟环境复验仍开放，WSL 记录不用于性能结论。
Native 7fb20bffdba35de7206d6be573fbc3957137eafa、Linux 239e595cbe725f18778eb0e23f49d6af67290344 的 4336 个项目文件字节一致，仅四个测试/注册文件变化，冻结后晚写六份结果文档。证据 build-v030-dev/evidence/write-deadline-integrity-tdd-20261017/，旧十份封存和历史另一个 build 的 49 个进程保留。[审校详情](v0.3.0-glm-takeover-review.md#50-2026-10-17-写超时测试假通过与完整池背压回归)和最新交接入口为后续依据。
本轮完整池大正文属于存活 Worker 响应路径；503 本地待完成内核写、池级非零配置覆盖、不同写阶段与退出/切换交叠、其他取消和资源、三类既有 workerd 差异/WPT profile、完整 sanitizer/fuzz/soak/native Mac/Hosted/T15–T17/全新双构建仍开放。两平台共 22 次执行/13 个测试名出现 TOKEN LEAK 诊断而通过，也包括本轮 host_worker_deadline_independent，详见 runtime-exit-observations.json；normalCleanup 只代表公开 wait 成功，不能证明逐 Worker 退出码 0 或 token 无残留，需独立终态/负向矩阵。T02/T04/T06–T08/T10–T17 与 0.3.0 总体未整体验收，根仓库未提交或发布。

<!-- capsid-review-phase-58 -->
## 第 58 轮本地验证更新（2026-10-17）

结果总览：`build-v030-dev/evidence/gb18030-workerd-compatibility-20261017/verification.json`，实现与边界说明见 `v0.3.0-glm-takeover-review.md` §58。Windows 原生 WPT 构建已执行；Hosted Windows 仍未配置 WPT。

- Windows 全量 449：435 通过、2 失败、12 跳过；Linux 全量 668：663 通过、2 失败、3 跳过，均零排除、exit=8。Linux root 重验 3/3、必选参考门禁 22/22、严格 profile 审核 complete=true；普通标准结果仍为失败。
- 两平台原始 WPT 均为 84 文件 / 14,060 子测，2 个文件的 3 条既有诊断及 1 项缺失预期保留；GB18030 275 项无 waiver。不要把 Linux 门禁写成原生 Windows workerd 已验证。
- 聚焦 Windows 6/6、Linux 7/7，包括真实 Host 编码模式和固定数据生成检查。单独选择可用 CTest 正则 `^(gb18030_.*|multibyte_indexes_generated|worker_wpt_encoding_legacy_mb_schinese_gb18030_gb18030_decoder_any_js)$`；source-integrity fixture 会自动加入。
- 新 Host 测试比较 368 组向量、2,568 次流式运行；本轮五种平台/模式的额外采样逐项比较全部 23,940 双字节和 1,587,600 四字节默认解码指针。真实旧 Worker 红测、15 项观察验证器和 3 项生成数据篡改控制均独立归档。穷举采样及 private 控制尚未全部纳入日常 CTest。

原始 GB18030 文本可按 lock 的 source_url 下载，再运行 `node tools/verify-gb18030-source.mjs <downloaded-index-gb18030.txt>`。数据生成审计使用 `tools/generate-gb18030-ranges.mjs <wpt-root> <output> --check` 和 `tools/generate-multibyte-indexes.mjs <wpt-root> <output> --check`，都不需要在构建中访问网络。

后续补按平台/WPT 配置适用的注册删除与命令替换控制、原生 reference、MSVC ASan、稳定时钟与 D005 HTML。当前完整运行的时钟监测保留 70119 样本，未放宽原始 30 ms 断言，未补跑 HR-Time 来替换结果。本轮没有新的 sanitizer 证据。


## Linux production and sanitizer sandbox profiles

`CAPSID_SANDBOX_PROFILE=production` is the default. It retains the production
syscall and Landlock policy. Release and isolation acceptance use this profile.

For Linux ASan/LSan qualification, explicitly configure
`-DCAPSID_SANDBOX_PROFILE=sanitizer -DCAPSID_ENABLE_ASAN=ON -DCAPSID_ENABLE_UBSAN=ON`.
This profile requires sanitizer instrumentation and is rejected on other OSes.
Its name is part of `buildFeatureFlags` and therefore the build provenance ID.
The C ABI layout is unchanged. Switching only the sandbox profile preserves
bytecode compatibility; ASan/UBSan compiler flags retain their existing
compatibility effect.

The diagnostic policy permits sanitizer tracer/symbolizer process operations
(clone/fork/exec/ptrace, waiting and signalling), executable memory mappings,
and read-only directory access. Landlock additionally permits reading `/proc`
and the worker executable, and reading/executing system symbolizer paths beneath
`/usr/bin`, `/usr/lib`, `/lib`, and `/lib64`. Application filesystem-write and
network-listener permissions are not added by this profile. This diagnostic
configuration has a different isolation boundary and is not the production
security policy.

`sanitizer_sandbox_clean` proves normal exit after installing resource limits,
no-new-privileges, Landlock and seccomp. `sanitizer_sandbox_leak` requires LSan to
report a deliberate 128-byte leak from a terminated thread. Both retain
`detect_leaks=1`, abort-on-error and fatal UBSan behavior. A sanitizer profile
result does not replace production policy negative controls; run both builds.

Keep runtime scratch on a private native Linux filesystem and use a short path
for tests that create UNIX-domain sockets. DrvFS directory permission semantics
and paths exceeding the native socket limit do not qualify managed-host tests.
