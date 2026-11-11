# FetchRPC v4 contract

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

本轮不修改公共 C ABI、v4 消息、能力位和配额。Promise 状态桥为私有只读观察；基础层历史输入连续性不等于本轮重新执行。

<!-- capsid-review-phase-84 -->
<a id="capsid-review-phase-84"></a>
## 第 84 轮状态（2026-10-20）：新增 fetch 边界已完成专项闭环

**upgrade38** 新增 TLS/SNI、真正的 IPv6、重定向、响应体错误与 Request/Response 转换的固定 **22 项**场景；双端真实 workerd 与七组当前产品各 **22/22**。Windows 普通/ASan、Linux 的完整 required gate 为 **57/57、57/57、72/72**，定向 CTest 为 **21/21、21/21、22/22**，两端 15 个 Node 校验模块各 **5169/5169**。注册 **534/535/759**，必需 **130/130/150**；未用注册数量代替产品执行。

比较器按字面语义检查原始 HTTP/完整 WebSocket 字节、每跳 nonce、证书/SAN、请求数量、前缀后错误、相同 VM 和服务停止前的源端结束。新增边界单模块 640 项，平台单模块 559 项，归档门禁单模块 202 项；组合的 `workerd_ws_gate_controls` CTest 为 456 项。相同错误出现在两端也必须拒绝。Response 的无关 redirect 值与 getter 均不得被观察，初稿的共享初始化回归已经修正。旧产物与初稿的真实反向结果、收集不完整状态和失败验证记录保留。

**整体 0.3.0 强兼容与发布仍未完成。** 包版本 0.2.2、补丁 58、输入 4466，未提交/推送/发布，未新跑冻结 ALL 或不筛选全量矩阵；最后全量仍是 compat27b 的 **1762：1733 通过、6 失败、23 跳过**。证据入口 `build-v030-dev/evidence/native-asan-qualification-20261018/upgrade38-final-audit.json`；详细修正、验证边界与剩余工作见 [第 84 轮接棒说明](v0.3.0-next-ai-handoff.md#capsid-review-phase-84)。下方旧轮次是历史，不应据此重复声明本轮缺口仍未实现。

本轮沿用现有 v4 消息与 C ABI、能力位、配额及关闭机制；仅修正主机 TLS SNI、共享地址配置解析、body 错误分类与 JS 转换行为。基础层上一轮实际执行输入逐文件未变，本轮仅报告连续性核验。Linux 的实际编译开关为 C11/C++11；MSVC 为 C11 与受支持的默认 C++14，没有宣称 Windows 实际使用不存在的 C++11 开关。

<!-- capsid-review-phase-83 -->
<a id="capsid-review-phase-83"></a>
## 第 83 轮：原生代理复用既有通道、额度和来源协议（2026-10-20）

upgrade37 实现 fetched 101 Response 的私有入站/出站 native coupling，**本轮未新增 C ABI、wire 布局或能力位**。沿用上一轮可选 CONNECTION_LIFETIME **128**、帧 **28**、追加 C 事件 **19**、第一方有效掩码 **251**；出站拨号 owner 不得发仅用于入站来源的 retain。接收消息以不可变租约与原 authority 转交，先预留发送额度，分片发出后才释放原租约；压力下暂停 credit，异常结束沿用取消/ACK。

网络 pump 与原 HTTP 应用来源的寿命分别管理：仅真实已 accept 的同源 JS owner 继续维持应用工作，native pipe 自身不会延长定时器/body/Promise 续体。三套底层 **19/19**，C/C++ 基础各 **6/6**；新增真实代理双端参考及七组产品各 **22/22**，完整 required gate **53/53、53/53、67/67**。旧能力/布局回归保持。更多交叉压力及整体 0.3.0 尚未验收，见 [当前交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-83)。下文为历史。

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

# FetchRPC v4 contract

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

Promise 创建标识独立于活动 job/resource 引用；HTTP 身份为弱引用，连接结束后撤销应用执行资格。已提交 101 但对端尚未 accept 时保留原有有期限、来源匹配的关联。旧 HTTP cancel/timeout、停放 reaction、分离定时器和 Upgrade 等待/释放断言继续通过。

**0.3.0 整体强兼容与发布验收仍未完成**，包版本仍为 0.2.2。完整实现、原始失败、产物索引及后续任务见[第 71 轮交接](v0.3.0-next-ai-handoff.md#capsid-review-phase-71)。以下第 70 轮及更早内容为历史记录。

<!-- capsid-review-phase-70 -->
<a id="capsid-review-phase-70"></a>
## 第 70 轮当前进度（2026-10-19）：异步回调与执行预算

当前候选 **async21c**，三套同字节输入的 strict ALL 构建通过；Windows Release、Windows ASan、Linux Release/LTO 的平台门禁分别 **29/29、29/29、37/37**，专项 CTest 分别 **36、37、39 项**，零失败、零跳过。项目 TSan **31/31**（110 个项目 C++ 输入，仍非完整依赖插桩）。

异步拒绝通知沿用所属连接/请求上下文，并在通知结束后释放额外引用；deadline 后禁止连接应用回调和延迟通知重入，私有清理 token 保持独立预算。原有 17 项 owner 生命周期、161 项共享异常和 HTTP cancel/timeout/分离延续的严格负向控制继续通过；本轮没有扩大回收窗口或放宽队列、变体及请求预算。

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
## Current checkpoint: handler15b synchronous exception snapshots

The three strict builds and actual platform reference gates (19/19/24) pass. The full unfiltered matrices retain their standard WPT failures and conditional skips; the current testing and handoff documents give exact counts. Release qualification remains unfinished and package version stays 0.2.2.

The exception name/message snapshot is captured before queued microtasks; the trusted error event follows the nested microtask checkpoint. The reviewed contract now has 55 shared cases and 1,520 API assertions on each official pinned asset. The 164 comparator controls and 25 actual fixture variants preserve baseline-first checking and exact diagnostics. Each independent product backend executes 12 original wire cases and 56 handler-error cases, including one explicit C17 retained-transport boundary. The final summary derives its count from actual observations; its correction was replayed on all 7 existing product backends without changing assertions or binaries. Original frozen full logs are retained.

This is not a complete exception clone implementation. Object cloning/stringification, throwing name getters, DOMException/AggregateError members, cause/custom properties/object graphs and broader global replacement inputs remain open. A separate pending restoration-timer lifetime observation is also retained. Windows reference SIGTERM/nonforced termination is recorded distinctly from Linux exit 0 and the required product Host exit 0. Project TSan 15/15 (104 project C++ inputs) and qualified root 3/3 on the same products do not close wider dependency or release qualification.

Current evidence: `handler15b-verification.json` and `handler15b-final-audit.json` in the 20261018 evidence directory. The pinned upstream [error serializer](https://github.com/cloudflare/workerd/blob/8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f/src/workerd/jsg/ser.c%2B%2B) and [WebSocket adapter](https://github.com/cloudflare/workerd/blob/8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f/src/workerd/api/web-socket.c%2B%2B) explain why broader cloning and message generation need separate coverage. No FetchRPC byte layout or capability policy changes in this checkpoint.

> Earlier checkpoints below are historical.

Status: the original v4 byte vocabulary (2026-10-11) and A2 layout v4.1
(2026-10-12) are the compatibility baseline. The **2026-10-13 audit corrections**
repair the complete head encodings and executable evidence below. The negotiated
**v4.2 confirmation amendment (2026-10-13)** adds type 27 and capability bit 4;
its pending-dial, result and terminal barriers are specified in §3.5.1 and
validated by independent per-direction two-endpoint transcripts. The
**2026-10-18 final-Close amendment** adds capability bit 5 and Close kind 3
without changing the A2 payload layout. Production capability advertisement
is specified in §3.5.1.

Types 15–18 are already allocated (`kAudit`, `kMemoryMetricsRequest/Response`,
`kLoadBinding`), and the assigned-channel payloads of types 19–26 remain unchanged. v3 bytes and ABI v7
remain unchanged. A new wire capability or layout requires a versioned
amendment and executable vectors as required by
[ROADMAP.md](../ROADMAP.md#v030-fetchrpc-v4-and-websocket-foundation).

The A2 layout supersedes the original draft channel payloads. Its JSON
object schema is `schema_version: 2`. The takeover audit found that the
sequence harness had not reliably executed; the corrected replay now runs
every step, validates complete payloads, and distinguishes its remaining
release blockers from the byte-layout baseline.
Audit-remediation inputs are recorded in
[v0.3.0-implementation-design.md](v0.3.0-implementation-design.md),
with executable contract work in task T05 of the
[GLM plan](superpowers/plans/2026-10-12-v0.3.0-remediation-and-delivery.md).

FetchRPC crosses the Host↔Worker process boundary and carries HTTP,
Upgrade, WebSocket, and Host-mediated service traffic. v4 adds
**duplex channels**; v3 remains exactly as shipped in ABI v7.

## 1. Framing and negotiation

### 1.1 Frame layout (unchanged from v3)

```text
offset  size  field
0       4     magic bytes 57 52 54 43         (0x43545257, LE)
4       2     version (u16)                  v4 frames: 4
6       2     type   (u16)
8       4     flags  (u32)
12      8     request_id (u64)
20      4     payload_size (u32)             <= 64 KiB
24      …     payload
```

### 1.2 Hello v4 (amended after the payload audit)

The host's `kHello` carries the worker startup configuration in v3; v4
appends a capability tail to that payload instead of replacing it (the
draft's fixed-87-byte shape ignored the config fields — amended
2026-10-12, recorded here):

```text
[ v3 Hello payload: startup config, variable length, >= 108-byte fixed prefix ]
-8      4     capabilities (u32 LE bitset, tail)
-4      4     reserved (u32, must be 0)
```

The worker answers its capabilities on `kReady`: in v4 the READY
payload appends `u32 worker_capabilities, u32 reserved(0)` after the
existing compatibility-id + sandbox-proof bytes. **Effective
capabilities are the conjunction** of both sides' bitsets; a side that
did not announce a feature must not emit its frames. Validation reads
the tail (last 8 bytes).

Capability bits (bit index → meaning):

| bit | name | effect |
| --- | --- | --- |
| 0 | `CAP_DUPLEX_CHANNEL` | channel frames (§3) are legal |
| 1 | `CAP_UPGRADE` | Upgrade accept/reject ops (§2) are legal |
| 2 | `CAP_HALF_CLOSE` | `kChannelHalfClose` is legal |
| 3 | `CAP_CHANNEL_LAYOUT_A2` | the A2 payloads (§3.6) are in force |
| 4 | `CAP_CHANNEL_CONFIRM` | v4.2 result/terminal ACK and pending-dial CANCEL (§3.5.1) |
| 5 | `CAP_CHANNEL_CLOSE_FINAL` | atomic final Close and transport barrier; requires CONFIRM (§3.5.1) |
| 6 | `CAP_FETCH_UPGRADE` | optional HTTP upgrade metadata/body channel (§3.2a); first-party basic integration verified, full lifecycle/proxy qualification pending |
| 7 | `CAP_CONNECTION_LIFETIME` | optional incoming local-owner retention after transport DISCONNECT (§3.2b); requires UPGRADE and CONFIRM |
| 8..31 | reserved | **must be zero**; a nonzero reserved bit or an unknown capability bit is a fatal protocol error |

**Dependency closure (v4.1):** `CAP_DUPLEX_CHANNEL`, `CAP_UPGRADE`,
and `CAP_HALF_CLOSE` each require `CAP_CHANNEL_LAYOUT_A2`.
`CAP_UPGRADE`, `CAP_HALF_CLOSE` and `CAP_CHANNEL_CONFIRM` additionally require
`CAP_DUPLEX_CHANNEL` and A2. Announcing `CAP_CHANNEL_LAYOUT_A2` without
`CAP_DUPLEX_CHANNEL` is a capability error. A bitset that violates the
closure is rejected as `invalid hello capabilities` — on both the
Hello and the READY tail.
`CAP_CHANNEL_CLOSE_FINAL` additionally requires `CAP_CHANNEL_CONFIRM`,
and therefore DUPLEX and A2; this closure is checked on both tails.
`CAP_FETCH_UPGRADE` additionally requires UPGRADE, CONFIRM and CLOSE_FINAL,
and therefore DUPLEX and A2 (complete minimal mask 123).
`CAP_CONNECTION_LIFETIME` additionally requires UPGRADE and CONFIRM,
and therefore DUPLEX and A2 (complete minimal mask 155). It does not require
FETCH_UPGRADE or CLOSE_FINAL. The known mask is 255; the first-party effective
mask is 251 because HALF_CLOSE is not advertised.

Allocation rule: a feature is usable only when both sides announce it.
Both announcements must satisfy the dependency closure, so their intersection
also satisfies it. A frame for a non-negotiated feature fails with
`channel frame without capability` even when its type is legal.

#### 1.2.1 Hello A2 limits extension (v4.1)

When (and only when) the Hello's capability tail carries
`CAP_CHANNEL_LAYOUT_A2`, a length-delimited extension sits **between**
the v3 startup payload and the capability tail:

```text
[ v3 startup payload, variable ]
u32     extension_size = 48
u16     extension_version = 1
u16     reserved = 0
u32     max_channels             1 ..= 1024
u64     max_message_bytes        1024 ..= 64 MiB (67108864)
u64     reassembly_budget_bytes  1 MiB ..= 256 MiB
u64     send_budget_bytes        1 MiB ..= 256 MiB
u32     close_timeout_ms         100 ..= 600000
u32     dial_timeout_ms          100 ..= 600000
u32     idle_timeout_ms          0 (= disabled) ..= 86400000
u32     per_channel_queue_bytes  64 KiB ..= 4 MiB
u32     capabilities             (tail follows immediately)
u32     reserved = 0
```

All fields LE. Validation, in order: `extension_size` must be exactly
48 and leave exactly 8 bytes (the tail) after it;
`extension_version` must be 1; `reserved` must be 0; every bound above
must hold; no trailing bytes may remain. Any failure is
`invalid hello extension`. A Hello that announces A2 without the
extension, or carries the extension without A2, is likewise
`invalid hello extension`. A valid Hello under A2 must also pass the
real v3 startup decoder on its prefix — an all-zeros placeholder is
not a valid startup payload.

### 1.3 Version negotiation matrix (deterministic rejection)

A parser rejects, as its **first** action on the offending frame, with
`kParseError` and the exact error string below; no partial state may
survive:

| wire | v3 parser (shipped) | v4 parser |
| --- | --- | --- |
| version 3 frame | parse per v3 | `unsupported protocol version` |
| version 4 frame | `unsupported protocol version` (frozen by `test_rejects_unknown_type_and_version`) | parse per v4 |
| version <3 or >4 | `unsupported protocol version` | same |
| v4 Hello with reserved ≠ 0 or unknown capability bits | (version rejected first) | `invalid hello capabilities` |

Peers never downgrade and never mix versions on one connection.
"Upgrading" a connection means a new connection.

## 2. Upgrade (requires `CAP_UPGRADE`)

An inbound HTTP request carrying `Upgrade: websocket` (after host
validation, including `Sec-WebSocket-Key`/`-Version` per RFC 6455) is
forwarded to the worker as a normal v3-shaped `RequestHead` **plus**
the frame flag `kFlagRequestUpgrade` and an A2 upgrade tail appended
to its payload — `u32 channel_id` (pre-assigned by the host at
admission; id authority stays with the host; no worker-minted ids, no
open round trip) and `u64 max_message_bytes`. The worker answers
exactly one of:

- `kResponseHead` with status 101 **and** `kFlagResponseUpgrade`,
  whose payload appends the A2 response tail — `u32 channel_id` (the
  same id) and `u64 accepted_max_message_bytes`; the effective channel
  message cap is `min(request tail, response tail)`. The HTTP response
  headers the host writes are the RFC 6455 accept handshake
  (`Sec-WebSocket-Accept`), produced by the host; the selected
  subprotocol is extracted from the worker's response headers by the
  host, which confirms it is a member of the offered list and exactly
  one.
- a normal non-101 response — reject; the host answers the upgrade
  with that response, the pending id reservation is released, and no
  channel exists. `kResponseEnd` follows as for any normal response.

The tail follows the **complete** legacy head. RequestHead has
`u16 method_size, method, u32 url_size, url, u16 header_count, headers`.
An upgrade ResponseHead has `u16 status, u16 status_text_size, status_text,
u16 header_count, headers`, then the 12-byte tail. Each header is
`u16 name_size, name, u32 value_size, value`. Status is not a u32;
status-text and header-count prefixes must not be omitted. Both heads use
the same nonzero HTTP request id. The response tail must commit the pending
channel belonging to that request. After 101, no ordinary ResponseEnd is sent;
HTTP accounting ends and channel traffic uses header request_id = 0.

The C ABI `upgrade_begin` reserves off-wire. The next `begin_request` or
`begin_bodyless_request` for that HTTP id sends the actual head and tail.
Queue refusal preserves the reservation and all credit for retry. Cancelling
an unannounced reservation performs local cleanup without emitting a frame
for an id the worker has never seen.

The successful 101 ResponseHead is the commit point from pending to
open. A non-101 response to an upgrade-flagged request releases the
reservation; no channel frame for that id may follow (violation
`unknown channel`). `kFlagResponseUpgrade` is mutually exclusive with
`kFlagResponseFixedBody` on the same frame. Any other response to an
upgrade-flagged request (status 101 without `kFlagResponseUpgrade`,
the upgrade flag on a non-ResponseHead, upgrade tails on responses to
non-upgrade requests) is a protocol violation
(`invalid upgrade response`).

## 3. Duplex channels (requires `CAP_DUPLEX_CHANNEL`)

### 3.1 Identity and lifecycle

- `channel_id` is a **u32 assigned by the host** (the worker never
  mints ids) from a monotonic counter starting at 1 within one IPC
  connection, in announcement order; 0 is never a channel, ids are not reused, and the
  `request_id` namespace of v3 is untouched. When the u32 space
  exhausts, new channels are refused and the supervisor replaces the
  worker in an orderly shutdown.
- The worker requests outbound channels with `kChannelOpen`
  (`channel_id = 0`, nonzero `dial_id`, target URL, and candidate
  subprotocols). Dial ids are an independent, monotonic u64 sequence;
  they cannot be reused within the IPC connection, including after rejection
  or cancellation. The host assigns the id when announcing the result and answers `kChannelAccept`
  (with the selected subprotocol) or `kChannelReject` (with a reason
  code and a bounded diagnostic). Inbound upgrades never send OPEN:
  the RequestHead upgrade tail (§2) is the open.
- Ownership (exactly one per channel, fixed at open): `handshake`
  (pending upgrade, pre-101), `duplex-connection` (post-101 inbound),
  `request`, `response-stream`, `standalone` (outbound dial), `tail`
  (reserved; creating one is refused in v0.3.0). The owner kind is
  carried in OPEN and drives cleanup (§3.4).

### 3.2 Frame table (types are stable; payloads below are A2, v4.1)

| type | name | direction | payload (A2) |
| --- | --- | --- | --- |
| 19 | `kChannelOpen` | worker→host only | `u32 channel_id(=0), u64 dial_id(!=0), u8 owner_kind, u8 channel_kind, u16 reserved(=0), u64 owner_request_id, u64 max_message_bytes, u32 url_size, url bytes, u16 protocol_count, [u16 size + bytes]*` |
| 20 | `kChannelAccept` | host→worker only | `u64 dial_id, u32 channel_id(!=0), u64 accepted_max_message_bytes, u16 selected_size, bytes` |
| 21 | `kChannelReject` | host→worker only | `u64 dial_id, u32 channel_id, u32 reason_code, u16 diagnostic_size, bytes` |
| 22 | `kChannelData` | both | `u32 channel_id`, then message fragment bytes (≤ 65532 per frame; flags = start/end/text) |
| 23 | `kChannelCredit` | both | `u32 channel_id, u64 credit` |
| 24 | `kChannelHalfClose` | both (requires `CAP_HALF_CLOSE`) | `u32 channel_id` |
| 25 | `kChannelClose` | both | `u32 channel_id, u8 close_kind, u8 flags, u16 code, u16 reason_size, reason bytes (≤ 123)` |
| 26 | `kChannelCancel` | both | `u32 channel_id(!=0), u32 terminal_reason` |
| 27 | `kChannelAck` | both (requires CONFIRM) | result/terminal acknowledgement (§3.5.1) |
| 28 | `kConnectionLifetime` | worker→host only (requires CONNECTION_LIFETIME) | `u32 channel_id(!=0), u32 retained(0 or 1)` (§3.2b) |

String prefixes: URL `u32`, subprotocol and diagnostic `u16`. Every
channel frame carries `request_id = 0` in the header; ids come from
the payload only. Grammar-level bounds (violation
`invalid channel payload`): truncation, trailing bytes, inconsistent
lengths, `reserved != 0`, `owner_kind`/`channel_kind` outside the
frozen enums, `protocol_count > 64`, any subprotocol or the
selected/diagnostic string `> 256` bytes, `max_message_bytes` outside
`[1024, 64 MiB]`, Data payload `< 4` bytes, and CANCEL
`terminal_reason > 5`.

Frozen enums:

```text
owner_kind: 0 handshake, 1 duplex-connection, 2 request,
            3 response-stream, 4 standalone, 5 tail (refused), 6..255 invalid
channel_kind: 0 websocket; 1 fetch-upgrade only with CAP_FETCH_UPGRADE;
              without that capability 1..255 invalid; 2..255 always invalid in OPEN
dial reason (REJECT): 0 unspecified, 1 egress-denied, 2 dns-failure,
            3 connect-refused, 4 tls-failure, 5 handshake-rejected,
            6 protocol-violation, 7 dial-timeout, 8 resource-limits,
            9 cancelled
cancel terminal_reason: 0 local, 1 disconnect, 2 timeout,
            3 shutdown, 4 resource-limits, 5 protocol-violation
```

OPEN direction rules (violation `channel open direction`): OPEN
arriving at the host must have `channel_id = 0` and `dial_id != 0`;
OPEN arriving at the worker, ACCEPT/REJECT arriving at the host, OPEN
with nonzero `channel_id` or zero `dial_id` are all rejected. A REJECT
for a dial that has not been assigned an id keeps `channel_id = 0`.

Types 15–18 stay allocated to `kAudit`, `kMemoryMetricsRequest`,
`kMemoryMetricsResponse`, and `kLoadBinding` (v3); v4 does not
renumber them. The Upgrade flags ride the existing head frames:
`kFlagRequestUpgrade` = bit 5 (value 32) on `kRequestHead`,
`kFlagResponseUpgrade` = bit 5 (value 32) on `kResponseHead`. Flag
namespaces are per frame type, as in v3. Data flags: `kFlagStart` (1),
`kFlagEnd` (2), `kFlagChanText` (4) on `kChannelData` only.

### 3.2a Optional FETCH_UPGRADE metadata (2026-10-20)

This extension is implemented in the codec, registry, additive C ABI and
first-party JS/Worker/Host bridge. First-party peers now negotiate bit 6.
Phase 78 verifies thirteen basic public fetch cases. Phase 81 verifies
nineteen shared-I/O-origin lifetime cases, including deferred incoming
local outgoing close after transport loss, with or without a sibling.
Complete native required gates and bounded retained-owner controls are
verified. Broader context graphs, managed generation retention, direct
proxy and transport qualification remain open; these bounded results do
not qualify the whole release.

With negotiated `CAP_FETCH_UPGRADE`, OPEN kind 1 appends to the unchanged
A2 prefix and empty subprotocol list:

```text
u16 metadata_version (=1), u8 redirect (0 follow / 1 manual / 2 error),
u8 reserved (=0), http_headers
```

The matching ACCEPT keeps an empty selected_protocol, then appends:

```text
u16 metadata_version (=1), u16 status (101 or 200..599),
u8 redirected (0/1), u8 reserved (=0),
u16 status_text_size, status_text bytes (<=1024),
u16 final_url_size, final_url bytes (nonempty, <=2048), http_headers

http_headers = u16 count (<=128),
               [u16 name_size, name bytes, u16 value_size, value bytes]*
```

The entire header encoding, including its count and length prefixes, is
bounded to 48 KiB. Names are nonempty ASCII HTTP tokens; values are counted
ByteString octets (no NUL/CR/LF or other field controls except HTAB).
Order and duplicates are retained, including separate Set-Cookie fields.
The complete payload still fits the 64 KiB frame bound. Truncation,
trailing bytes, bad versions, reserved fields or bounds are
`invalid channel payload`.

Metadata is required exactly for the matching kind-1 OPEN and forbidden
on ordinary constructor dials. Old codec callers default to no new
capability and preserve exact old bytes. Status 101 resolves to ordinary
WebSocket transport while retaining its fetch authority. Other statuses
resolve to registry-only kind 2 (HTTP body); kind 2 is never legal in OPEN.
Body DATA is binary Host-to-Worker only; credit is Worker-to-Host only.
204/205/304 admit no DATA. Successful body EOF is Host CLOSE_FINAL without
WebSocket code/reason and cannot truncate an open DATA message; its
transport ACK and cancellation barriers remain mandatory.

`capsid_worker_dial_accept_http()` takes a separately sized/versioned
descriptor, synchronously copies counted inputs, and transactionally
preserves pending state on queue refusal. Existing ABI-v7 layouts and the
ordinary dial_accept operation remain unchanged. The old typed DIAL view
rejects a new kind rather than silently discarding HTTP fields. The first-party
Host now performs capability-aware typed propagation through the versioned
HTTP descriptor. Basic public fetch/101/non-101/stream/abort/redirect behavior
is verified in phase 78. Phase 81 verifies nineteen shared-I/O-origin
lifetime cases and the complete native required gates. Incoming transport
EOF retains its locally open outgoing owner; local close drains queued
jobs before cancelling the final origin's native resources. Retention has
finite admission and explicit shutdown controls. Broader HTTP/context/
resource graphs and direct response proxy coupling still require
implementation or qualification.

### 3.2b Optional CONNECTION_LIFETIME (2026-10-20)

With negotiated bit 7, type 28 has exactly eight little-endian payload bytes,
zero flags and zero header request id. State 1 retains an already committed
incoming duplex owner after Host DISCONNECT and before its terminal ACK.
Pending upgrades, outbound dials, other terminal reasons, duplicate retains,
unknown ids and malformed states are rejected without changing the set.
The retained set is separate from physical channels and bounded by
`2 * max_channels`. Network ACK still releases physical capacity.

State 0 releases an existing retained id, including after its wire channel
was erased. Unknown or duplicate releases are rejected. Worker emits retain
before cancellation ACK when the incoming local outgoing direction remains
open; local close and the queued-job checkpoint finish the owner and emit
release. Worker teardown clears the set. This feature adds no network data,
credit or WebSocket close frame and does not change their existing formats.

ABI v7 appends `CAPSID_EVENT_CONNECTION_LIFETIME = 19` without changing any
existing structure layout. Its payload is the validated eight-byte control,
and `credit` is the range-checked total retained-owner count. The Host consumes
it as generation liveness and publishes physical plus retained ownership as
one atomic value; existing physical-channel metrics keep their meanings.
The original generation drain deadline still overrides retention.

Old embedders omitting bit 7 receive no type-28 frame or event 19. Legacy
Hello masks 0/31/63 preserve READY masks 0/27/59; new masks
123/155/251/255 produce 123/155/251/251. Matching first-party builds request
251. Older workers can reject this new capability; no automatic downgrade
or compatibility with an old worker advertising unknown bits is implied.

### 3.3 Credits and buffering

- Each direction maintains an independent u64 byte credit over
  **message bytes** (Data payload minus the 4-byte id prefix). Data may
  be sent only when credit ≥ fragment bytes; sending without credit is
  a protocol violation (`channel credit underflow`).
- Initial credit (both directions): 64 KiB (65536). Credit is
  replenished by `kChannelCredit` (additive; a replenish that would
  overflow u64 is `channel credit overflow`). Replenish arrives when
  the receiver has accepted a fragment into reserved reassembly space
  or completed the network write — not merely on receipt.
- With initial 65536 plus an additional 65536 grant, sending 65537
  message bytes is legal when no more than 65535 of that total credit was
  already consumed: credit arithmetic is over the running
  balance, not the initial window.
- Per-channel receive buffering is capped at 4 MiB (v3
  `kMaxBufferedBytes`); a sender pushing a peer over the cap is
  throttled by credit exhaustion, never by silent drop.
- Effective message cap per channel: `min(OPEN.max_message_bytes,
  ACCEPT.accepted_max_message_bytes)` (dial) or `min(request tail,
  response tail)` (upgrade), further bounded by the Hello A2
  `max_message_bytes`. A complete message larger than the cap is a
  protocol violation (`message size exceeded`). Frames carry at most
  65532 message bytes; larger messages MUST be fragmented.

### 3.4 Ownership cleanup matrix

| event | request-bound | response-stream-bound | tail-bound | standalone |
| --- | --- | --- | --- | --- |
| client disconnect | host sends `kChannelClose(1006-substitute code 1001)` then frees | same | survives until tail ends | host sends Close, frees |
| request timeout | host sends `kChannelClose(1001)`, frees | same | per tail deadline | unaffected |
| worker shutdown (clean) | worker sends Close for every open channel before `kShutdown` | same | same | same |
| worker replacement | host closes channels with code 1001, does not deliver to the old worker | same | same | same |
| peer Close received | deliver Close event, channel freed after both directions quiesce | same | same | same |

Half-close (`kChannelHalfClose`) ends one direction; the channel frees
when both directions are closed or half-closed in baseline A2. With CONFIRM,
HalfClose stops DATA only: CREDIT may still follow it, and the entry remains
until both full Close markers or a terminal ACK. For WebSockets,
half-close maps to "stop sending" (the JS API does not expose it in
v0.3.0; it exists on the wire for transport correctness).

### 3.5 Sequencing rules

- `kChannelData`/`Credit`/`HalfClose`/`Close`/`Cancel` referencing an
  unknown or already-freed id: protocol violation
  (`unknown channel`). Internal tombstones may absorb provable late
  completions of already-finished async work; they never excuse a wire
  frame that references a dead id.
- State machine per channel (worker-side dials mirrored):
  `dialing → (open | rejected)` for outbound dials;
  `pending → open` on the 101 ResponseHead for upgrades (a non-101
  response releases the pending reservation);
  `open → closing → closed` on Close/CANCEL. In `open`, each direction
  is independently `open` or `half-closed`.
- Frame order per channel is strict: (reservation/accept) → Data* →
  (HalfClose) → Close. Violations (`channel sequence`): Data on a
  pending (uncommitted) upgrade id or provisional v4.2 ACCEPT; a second Open for a live dial;
  Data after the **sender's** Close or HalfClose; a second
  same-direction HalfClose; a second same-direction Close.
- Fragmentation: a first Data fragment without `kFlagStart`, a
  `kFlagStart` while a message is open (nested start), a non-final
  fragment after `kFlagEnd`, a message whose fragments disagree on
  `kFlagChanText` (message type), or an end-flagless final fragment
  sequence that never closes: violation (`fragmentation`).
  `start → continuation → end` is the legal multi-fragment shape; a
  single frame with start|end set is a legal single-fragment message.
- Simultaneous close is legal: each side sends its own Close; the
  first Close received moves the channel to closing, and the peer's
  Close completes it. Duplicate Close from the **same** side is
  `channel sequence`.
- CREDIT is additive by definition (duplicates are simply two grants);
  no other frame is idempotent on the wire.

### 3.5.1 Confirmation amendment (v4.2, 2026-10-13)

`CAP_CHANNEL_CONFIRM` (bit 4) requires DUPLEX + A2 and is effective only if
both endpoints advertise it. Without it, ACK and pending-dial CANCEL fail
`channel frame without capability`. Production WebSocket adapters must require
this bit; baseline A2 alone lacks safe cancellation of an unassigned dial.
When the host offers CONFIRM, the production worker advertises DUPLEX, UPGRADE, A2 and CONFIRM (27), adding CLOSE_FINAL, FETCH_UPGRADE and CONNECTION_LIFETIME only when offered by the host and valid under their dependency closures (first-party mask 251). Without CONFIRM it advertises zero feature bits. HALF_CLOSE is not advertised.

`kChannelAck = 27`, zero flags and zero header request id, has exactly:

```text
u32 channel_id
u64 dial_id
u32 kind: 0 dial-result, 1 cancel, 2 transport-close
```

| kind | identity | direction and meaning |
| --- | --- | --- |
| 0 | nonzero dial id; assigned channel id for ACCEPT, zero for REJECT | worker→host: result consumed; ACCEPT becomes Open |
| 1 | assigned channel id / dial id zero, OR channel id zero / pending dial id | opposite side to the CANCEL sender; its producer has stopped |
| 2 | assigned channel id / dial id zero | opposite side to a transport Close; its producer has stopped |

Unknown kinds, ambiguous/empty identities, trailing bytes and malformed lengths
are `invalid channel payload`. Wrong result/pending-cancel ACK direction is
`channel open direction`. An unsolicited, duplicate or mismatched ACK is
`channel sequence` while the correlation exists, `unknown channel` after it is
reclaimed. Confirmations are never silently idempotent.

**Result barrier.** OPEN keeps a bounded dial record. ACCEPT assigns a fresh
host id but stays `AwaitingAck`; neither direction emits DATA/CREDIT/HalfClose
or assigned CANCEL until worker ACK kind 0 is queued. Host must receive that
ACK before producing DATA. REJECT retains only a result correlation until the
same ACK (channel id zero). Host cannot start an assigned terminal operation on
a provisional ACCEPT; it retains a transport failure and terminates after the
result barrier, or aborts IPC when the confirmation deadline expires.

**Pending cancellation.** Only the worker may cancel a locally unresolved
OPEN. Its CANCEL is the versioned 16-byte form:

```text
u32 channel_id = 0
u32 terminal_reason                 // unchanged enum 0..5
u64 dial_id != 0
```

The worker stops its dial producer and retains the correlation. Host receiving
it cancels resolver/connect/TLS/handshake IO and queues ACK kind 1 with channel
id zero and that dial id. After observing CANCEL, host must emit no further
ACCEPT/REJECT. A result already queued on the host stream may cross CANCEL;
the cancelled worker validates it, suppresses JS delivery and does **not**
send result ACK. An accepted result creates only provisional drain state,
never a revived JS connection. No DATA can precede result ACK, so provisional
connections have no unbounded network-to-worker queue. Host cancellation ACK
is ordered after every previously queued result. Both sides erase provisional
channel/result state at that marker. A processed result must use its ordinary
result ACK and, if accepted, subsequent assigned-channel cancellation instead.

**Assigned cancellation / transport failure.** The existing assigned CANCEL
remains exactly 8 bytes. CANCEL or transport Close ends its sender's producer,
releases owned queues and enters Cancelling. The opposite stream may contain
legal DATA/CREDIT/Close already queued before it learned about the terminal
marker. Validate that stream's fragmentation, credit and cap; drain its bytes
without payload allocation or JS callbacks. The receiver stops its producer
and queues the corresponding ACK after its earlier writes. The initiator
retires state when it consumes that ACK. Simultaneous terminal operations need
both ACKs; sending an ACK alone cannot erase one's own outstanding terminal
request. No new DATA/CREDIT/HalfClose/Close may be produced after observing a
CANCEL; the ACK defines the end of the peer's stream.

**Ordinary Close / HalfClose.** Close ends the sender's DATA and CREDIT stream,
not the opposite stream. A first Close must preserve opposite-direction
fragment validation state until that sender ends or confirms its stream.
Queued DATA crossing Close is valid and remains deliverable until the peer's
Close ends that receive direction; DATA
following the same sender's Close is invalid. After sending full Close, the
only further channel frame permitted is an ACK for a crossed peer terminal
operation. In CONFIRM mode both half-closed directions still keep the control
stream; mixed Close/HalfClose is not terminal until the second full Close or
confirmed CANCEL/transport failure.

**Atomic final Close (2026-10-18).** `CAP_CHANNEL_CLOSE_FINAL` (bit 5)
requires CONFIRM and is used only when both peers negotiate it. Close kind
`3` carries the same sendable code/reason (or absent code) as ordinary Close,
but also enters Cancelling and requires a transport-close ACK (kind 2).
The network adapter writes previously accepted messages and this Close in
order, stops the actual socket without waiting for a peer Close reply, then
acknowledges termination. Write failure and existing write/close deadlines
also tear down the socket before that ACK. Synthetic codes 1006/1015 remain
invalid for kind 3 and must never be written to the WebSocket wire.

This is one first Close marker, not a second Close following an ordinary
Close. Duplicate Close and DATA after the sender's Close remain invalid.
An ordinary peer Close crossing this final marker cannot reclaim the channel
until its terminal barrier is confirmed. Baseline A2 and CONFIRM peers that
did not negotiate bit 5 reject kind 3 with `channel frame without capability`.
The payload layout and existing kinds 0/1/2 retain their previous meaning.

2026-10-13 lifecycle correction (v4.2.1, no byte-layout change): local ordinary
Close must preserve owned opposite-direction assembly and message delivery.
The pinned workerd `lifecycle.mjs` probe verifies reception after local Close
and the final message from an `allowHalfOpen` close callback (7 assertions).
Suppression applies to CANCEL/transport terminal barriers, not ordinary Close.

**Bounds and deadlines.** The pending/result map is bounded by max-channels, separately from the
admitted-channel bound of the same size; rejected results reserve no live connection.
No per-completed-id tombstone is retained. OPEN uses the dial timeout; a result,
CANCEL, transport Close or incomplete full Close uses the close timeout. The
endpoint advances a monotonic clock. Missing confirmations expire the entire
IPC stream, release queues/correlations and forbid subsequent application:
`channel confirmation timeout`. This explicit failure prevents dropping a
record and later treating a legitimate queued ACK as an unrelated frame.
Adapters must drive the clock and prioritize bounded terminal writes. A
WOULD_BLOCK response spends neither credit nor confirmation state.

`tests/test_channel_races.cc` has separate host/worker registries and two FIFO
queues, independently scheduled reads: CANCEL before result, ACCEPT/REJECT
crossing CANCEL, result-before-DATA, queued fragmented DATA crossing Close,
assigned CANCEL and transport Close, simultaneous CANCEL, Close/CANCEL,
HalfClose/CREDIT, duplicate/unknown/mismatched identities and deadline cleanup.
The raw-peer C ABI test repeats pending/crossed cancellation over real IPC,
checks retry after a full writer and verifies a later dial on the same stream.

### 3.6 Close kinds and codes (v4.1)

`kChannelClose` payload freezes:

```text
u8 close_kind: 0 initiated (this side started the Close handshake)
               1 observed  (a peer Close frame was received)
               2 transport (the transport died without a Close)
               3 final     (write Close then end transport; requires bit 5)
u8 flags:     bit 0 (1) code-present; bits 1..7 must be zero
u16 code:     meaningful only when code-present, else must be 0
u16 reason_size + bytes (≤ 123, UTF-8 per the dossier)
```

Code matrix (violation `invalid close code`):

| close_kind | code when present |
| --- | --- |
| 0 initiated | 1000–4999 except 1004, 1005, 1006, 1015 (the sendable set) |
| 1 observed  | 1000–4999 except 1004, 1005, 1006, 1015 (what a lawful peer may put on the wire) |
| 2 transport | 1006 or 1015 only (synthesized observations) |
| 3 final | the sendable set, as for initiated Close; requires negotiated bit 5 |

Codes 1005/1006/1015 are receive-side observations and must never be
sent as network Close frames; on the IPC plane 1006 appears only with
`close_kind = 2`. A code-present flag with `code = 0`, or any
code outside the matrix, is `invalid close code`.

The network adapters use sendable Close payloads for WebSocket Close frames;
kind 2 is an IPC transport observation and emits no network Close frame.
Kind 3 also ends the actual transport after accepted frames drain and
requires the terminal ACK described above. Code and reason limits follow
the workerd dossier (`docs/workerd-websocket-semantics.md` §4).

### 3.7 Error table (v4.1, exact strings)

| string | scope | trigger |
| --- | --- | --- |
| `unsupported protocol version` | connection | version mismatch (§1.3) |
| `invalid hello capabilities` | connection | unknown/reserved bits, closure violation (Hello or READY tail) |
| `invalid hello extension` | connection | A2 limits extension malformed or out of bounds |
| `invalid channel payload` | frame | payload grammar: truncation, trailing bytes, lengths, enums, reserved, bounds |
| `channel open direction` | frame | OPEN/ACCEPT/REJECT direction or id misuse |
| `channel credit underflow` | channel | Data without sufficient credit |
| `channel credit overflow` | channel | replenish exceeding u64 |
| `message size exceeded` | channel | complete message over the negotiated cap |
| `unknown channel` | channel | id not live (never existed, freed, rejected, cancelled) |
| `channel sequence` | channel | order violations (§3.5) |
| `fragmentation` | channel | message assembly violations (§3.5) |
| `invalid close code` | frame | close kind/code matrix violation |
| `invalid upgrade response` | request | upgrade answer shape violations (§2) |
| `channel frame without capability` | frame | frame type legal but feature not negotiated |
| `channel confirmation timeout` | connection | a bounded dial/result/terminal confirmation did not arrive |

Channel-scoped errors tear down the offending channel; frame-scoped
grammar errors and connection-scoped negotiation errors tear down the
connection. Errors surface to embedders through REJECT, CANCEL, or the
existing kError channel with explicit scope — never silently.

## 4. Ping/pong

Owned entirely by the host transport; no FetchRPC frame exists. The
worker never sees pings (dossier §5).

## 5. Test vectors

Golden encodings and sequences live in
`tests/fixtures/fetchrpc-v4/vectors.json` (`schema_version: 2`,
schema documented in the file). The harness
`tests/test_fetchrpc_v4.cc` parses it with a real JSON parser
(vendored jansson), rejects missing files, wrong schemas, and
unexecuted sequences, asserts the shipped v3 parser rejects every
v4-versioned vector (the never-mix-silently boundary), round-trips
every vector through the v4 parser, and executes every sequence
against an independent reference state model (test-local; the
production registry repeats the same sequences in T06 and does not
substitute for it). Sequences cover: negotiation accept/reject matrix
(including the closure rules and A2 extension negatives), upgrade
accept and reject (with reservation release), fragmented messages,
credit exhaustion/replenish/overflow and the 65536+65536 window,
half-close, simultaneous close, unknown channel, duplicate close,
out-of-order dial completion, multi-channel interleave, message-cap
violations, cancel, and capability gating.

## 6. Settled design decisions (plan D1–D6)

| # | decision | outcome |
| - | --- | --- |
| D1 | WebSocket framing lives in the **host** (minimal RFC 6455 codec, property-tested). The worker never frames; txiki's `ws.c` stays unexposed (it would bypass egress mediation). |
| D2 | `channel_id` is **host-assigned**, u32, monotonic from 1; the worker only requests ids (0). |
| D3 | Half-close is a **dedicated frame** (`kChannelHalfClose`); the v0.3.0 JS API does not expose it. |
| D4 | Ping/pong is **transport-internal** on both planes; no FetchRPC frame, no JS surface. |
| D5 | ABI stays **v7, additive** (`capsid_worker_upgrade_*`, `CAPSID_EVENT_CHANNEL_*` on the event pump). Escalate to v8 only with a recorded embedder-blocking reason. |
| D6 | `WebSocketPair`/`WebSocket` ship in **`CAPSID-MIN`** with the workerd method surface; v0.5 adds `env`/`ctx` around it, not a different WebSocket. |

## 7. ABI surface (informational)

Additive v7 entries expected: `capsid_worker_upgrade_respond(...)` and
channel event pump entries on the existing event stream
(`CAPSID_EVENT_CHANNEL_*`). Escalation to ABI v8 only if embedders
cannot express duplex pumping additively.

## Executable-contract corrections (2026-10-13)

- Corrected the magic annotation and encoded complete legacy head prefixes in
  the vectors, preserving frame types and v3 bytes.
- Clarified monotonic id announcement, independent u64 dial ids, request/response
  ownership matching, and terminal reservation cleanup.
- Enforced zero channel-frame header request ids, zero A2 OPEN flags, bounded
  REJECT reason codes, and no reason on an empty initiated/observed Close.
- Repaired JSON lifetime and reset each sequence target before replay. All
  steps, including steps after a rejected frame, compare actual outcomes.
  The JSON schema remains v2 because its object shape is unchanged; the schema
  description records the corrected fixture revision.
- The v4.2 confirmation amendment adds a negotiated result barrier and terminal
  barrier, with two-peer Close/cancel transcripts and bounded deadline cleanup.
  The worker IO core now validates startup/READY/steady-state and releases owned
  queues on shutdown; native JS connection roots and network adapters remain pending.

Inbound upgrade cancellation uses the assigned-channel CANCEL barrier after
the RequestHead announcement. A queued opposite-direction 101 for that
handshake is validated and suppressed, then the terminal ACK removes the
correlation. An HTTP error/non-101 answer releases an unresolved handshake;
a committed duplex channel survives ordinary HTTP token cleanup.

<!-- capsid-review-phase-64 -->
<a id="capsid-review-phase-64"></a>
### 2026-10-18 transport drain and peer compatibility

The Host drains complete accepted socket writes before acknowledging a worker transport Close, without adding a network Close frame. Existing write/close deadlines still bound failure cleanup. The final-Close kind retains the Close frame and does not wait for a peer reply.

This optional capability does not change old unknown-bit rejection policy. First-party Host/Worker are delivered as matching builds; old clients omitting bit 5 negotiate their previous feature set and receive no kind 3. An old worker may reject a new Host's HELLO, and no automatic wire downgrade is claimed.
