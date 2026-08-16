# 许可证与商业边界

Capsid 采用“开放接口 + 保护核心 + 私有企业层”的三层结构。目标不是禁止
Fork，而是让 Fork 无法轻易替代官方项目。

## 当前边界

| 范围 | 许可证 |
| --- | --- |
| Core：`src/`、`cmake/`、`tools/`、构建与测试代码 | FSL-1.1-Apache-2.0，自首次发布起两年后自动转为 Apache-2.0 |
| 开放接口：`include/`、`docs/`、`examples/` | Apache-2.0 |
| `vendor/` | 各上游项目原许可证 |
| Capsid 名称 / logo | 商标，按 TRADEMARK.md 使用 |
| 企业层（control plane、fleet、安全情报等） | 不放入本仓库 |

完整 FSL 文本见 [../LICENSE](../LICENSE)，Apache-2.0 文本见
[../LICENSES/Apache-2.0.txt](../LICENSES/Apache-2.0.txt)。

## FSL 核心含义

- 允许：个人、内部、教育、研究、专业服务、自建部署、修改与 Fork；
- 限制：不能把 Capsid 核心作为商业产品或服务直接提供，替代 Capsid 官方
  产品或官方基于该软件提供的其他产品；
- 两年后当前版本自动获得 Apache-2.0 许可。

## 历史版本

v0.1.0–v0.1.3 已按 MIT 发布，已发布的 tag 不因本变更被收回，仍可依据
原 MIT 条款使用与 Fork。新许可证边界自后续版本起生效。

## 为什么这样设计

- C ABI、FetchRPC、policy schema 等接口保持 Apache-2.0，降低生态与集成
  摩擦；
- 核心 runtime/sandbox 使用 FSL 保护商业窗口，同时保留最终转开源承诺；
- 真正的长期护城河是开发速度、官方认证、商标与私有企业层，而不是删除
  Fork 按钮。

## 贡献

贡献者保留其代码版权，同时授予项目再许可与商业授权所需的权限，见
[贡献者协议](legal/CLA-individual.md) 与 [企业贡献者协议](legal/CLA-corporate.md)。
代码贡献需在提交中附 `Signed-off-by`（DCO）。

## 商业授权与认证

- 需要绕过 FSL 限制的组织可申请商业许可证；
- “Capsid Certified Runtime” 需要官方 conformance 与安全测试，见
  [certification.md](certification.md)。
