# 双 A/B 验收证据（返修批次，fresh 构建）

## A/B 1: a148005 vs current —— 性能悬崖修复
- bytes65537@64c: a148005 177 QPS（3 errors，挂死）→ current 1035 QPS（零错误）—— 5.8x
## A/B 2: 3f7e8b2 vs current —— 返修无回退
- bytes64k@64c: 1071 vs 1070（0%）
- bytes65537@64c: 1063 vs 1035（-2.6%）
- json@64c: 1638 vs 1642（+0.2%）
- json16k@64c: 1129 vs 1132（+0.3%）—— 无回退

结论：悬崖修复确认；返修批次（phase/OutboundBuffer/测试强化）无回退；
1K/16K 档相对修复前基线（1594/1134）亦无回退。每侧 manifest（commit、
二进制 SHA、loadgen/bundle SHA、参数）、profile（CPU ticks/RSS）与原始
样本（samples.*.jsonl）均保留在本目录。
