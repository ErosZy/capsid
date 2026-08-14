# 三栈对比基准：capsid+hono vs php-fpm+nginx+slim vs python+flask+gunicorn

2026-08-13。目标：三种「应用框架 + 服务宿主」组合在固定 payload 矩阵下的
吞吐/延迟对比。沿用仓库既有 four-stack 协议的公平性规则与证据约定
（`bench/compare-four-stacks.sh`、`bench/loadgen`、`docs/performance-benchmarks.md`
的结论门槛），只换 stack 组合与 workload 矩阵。

## 对比对象

| 栈 | 组合 | 进程形状 |
|----|------|---------|
| capsid | capsid-host static-pool `--workers 2` + capsid-worker + hono | 1 host + 2 worker |
| php | php-fpm 8.5.8（pm=static max_children=2）+ nginx + Slim | 1 nginx + 1 fpm-master + 2 fpm-worker |
| python | Flask + gunicorn 2 sync workers | 1 master + 2 worker |

双进程协议（每栈 2 个执行单元）与 four-stack 脚本一致；gunicorn sync worker
是仓库既有 flask 夹具（`bench/bench-apps/flask_app.py`）选定的形态。

## Workload 矩阵

12 个格子：**1k / 8k / 16k / 32k × JSON / bytes / stream**。

- 路由约定沿用 loadgen 的 `/@capsid/orders/bench/{json,bytes,stream}[{8k,16k,32k}]`
  （无后缀 = 1k；capsid 经 `--routing path` 剥离 `/@capsid/orders` 前缀后落到
  app 的 `/bench/*`）。
- 三栈 payload **逐字节对齐**：
  - json：`{"status":"ok","app":"<stack>","pad":"x"*N}`，N 使正文 ≈ 1k/8k/16k/32k
  - bytes：`0x61 × 1024/8192/16384/32768`，`application/octet-stream`
  - stream：分 3 块发送、合计 `0x62 × 1024/8192/16384/32768`，chunked 编码
- 每栈应用只做「预计算 payload + 写出」，不在请求路径上重新序列化/分配
  （hono 用 `new Uint8Array().fill()` 每请求构造，flask/slim 预计算字符串）。

## 公平性协议

1. **常驻**：三栈在整轮跑测前启动、跑完才拆，不逐格冷启动。
2. **锁核**：loadgen `taskset` 固定 2 核（6-7）；被测栈不锁核（four-stack 协议：
   loadgen 永不抢核，栈间共享同一主机其余资源）。协议与既有 four-stack 运行一致。
3. **轮转**：workload 间轮转起始栈顺序（offset 0..2），抵消主机随时间漂移。
4. **每格**：3 轮，warmup 3s + measured 8s，conns=64（inflight=64）。
5. **正确性门**：每轮 loadgen 写 correctness verdict（bytes/stream 校验长度+字节，
   json 校验状态与可解析性）；errors>0 或 timeouts>0 的轮次标红。
6. **结论门槛**（沿用 `docs/performance-benchmarks.md`）：均值 + CV≤7% 才作
   容量结论；CV 超标的格子只报「观察样本」，不作排名依据。
7. **环境注记**：php 腿在 docker 容器内（本机 Alpine 无 root、无 php 包），
   python 腿在本地 venv，capsid 腿本地裸进程。容器网络为 bridge+端口映射。
   该偏差沿袭 four-stack 脚本自身的多容器设计，写入 manifest 而不作修正。

## 组件改动

1. **loadgen 扩展**（`bench/loadgen/main.go`，Go 工具链可用）：新增
   `json8k/bytes8k/stream8k/json32k/bytes32k/stream32k` 六个 workload，
   期望长度 8192/32768、字节 0x61/0x62；json 无长度断言（与既有 json/json16k 一致）。
2. **hono bench app**：新建 `bench/bench-apps/hono-bench.mjs`（12 路由，payload
   对齐），用 esbuild（vendor/txiki.js 已带）打成 bundle。
3. **flask app 扩展**：`bench/bench-apps/flask_app.py` 增加 bytes/stream/8k/32k
   路由；venv 装 flask+gunicorn。
4. **slim app 扩展**：`bench/bench-apps/php-slim/public/index.php` 同样增加；
   容器内装 nginx + composer vendor。
5. **runner**：`bench/compare-three-stacks.sh` —— 以 four-stack 脚本为模板，
   `STACKS=capsid php python`、`WORKLOADS` 可参数化、3 轮、2 核 loadgen。

## 证据与产出

- 每轮一个 `samples.<side>.<workload>.c64.r<round>.jsonl` +
  `correctness.*.json`，跑完 `python3 bench/summarize4.py <out>` 出均值表。
- 产物目录 `bench/results/three-stack-<timestamp>/`，含 manifest
  （二进制 sha256、进程形状、容器/venv 版本、锁核布局）。
- 结论只写「观察到的样本」除非该格 CV≤7% 且 errors=0。

## 明确不做

- 不做 64k 档（既有 json64k 夹具保留但不在本矩阵）。
- 不做 conns=1 单连接延迟腿（four-stack 脚本有，本次矩阵未请求，可后续补）。
- 不做 ruby/fastapi 腿。
- 不调优任何一栈的默认配置之外参数（除双进程协议本身）。
