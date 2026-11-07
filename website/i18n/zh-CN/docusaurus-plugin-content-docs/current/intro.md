---
sidebar_position: 0
---

# 开始

**Fulla** 是以 C++17 构建的高性能开源身份与访问管理（IAM）核心：生产级 OAuth2/OIDC
授权服务器，完整覆盖用户认证、MFA、WebAuthn、RBAC 与多租户——既可作为**开箱即用的
产品**（Docker/Helm）部署，也可作为**可嵌入的 C++ SDK**（`find_package(fulla-*)`）集成。

快速入口：

| 你想… | 去 |
|---|---|
| 五分钟了解架构 | [架构总览](architecture/architecture-overview.md) |
| 跑起来试试 | [Docker 部署](operate/docker-deployment.md) · README 的 [Quick Start](https://github.com/voidvec/fulla#quick-start) |
| 把 OAuth2 引擎嵌进你的 C++ 项目 | [SDK 集成指南](sdk/sdk-integration-guide.md) |
| 用任意语言调 HTTP API | [API 参考](domains/api-reference.md) · [OIDC 集成](domains/oidc-guide.md) |
| 部署到生产 | [生产部署](operate/deployment.md) · [配置指南](operate/configuration-guide.md) |
| 理解某个设计为什么是这样 | [ADR 决策记录](adr/ADR-0001.md) |
| 给项目贡献 | [贡献区](contribute/testing-guide.md) |

本站内容直接来自[仓库的 docs/ 目录](https://github.com/voidvec/fulla/tree/master/docs)
（单一事实源，零拷贝）；发现错误请提 PR 修改仓库文档，站点随 master 自动重建。
