---
sidebar_position: 0
---

# Getting Started

**fulla** is a high-performance, open-source identity and access management (IAM) core built in C++17: a production-grade OAuth2/OIDC authorization server with full coverage of user authentication, MFA, WebAuthn, RBAC, and multi-tenancy — it can be deployed as an **out-of-the-box product** (Docker/Helm) or integrated as an **embeddable C++ SDK** (`find_package(fulla-*)`).

Quick entry points:

| If you want to… | Go to |
|---|---|
| Understand the architecture in five minutes | [Architecture Overview](architecture/architecture-overview.md) |
| Get it running | [Docker Deployment](operate/docker-deployment.md) · [Quick Start](https://github.com/voidvec/fulla#quick-start) in the README |
| Embed the OAuth2 engine in your C++ project | [SDK Integration Guide](sdk/sdk-integration-guide.md) |
| Call the HTTP API from any language | [API Reference](domains/api-reference.md) · [OIDC Integration](domains/oidc-guide.md) |
| Deploy to production | [Production Deployment](operate/deployment.md) · [Configuration Guide](operate/configuration-guide.md) |
| Understand why a design is the way it is | [ADR Decision Records](adr/ADR-0001.md) |
| Contribute to the project | [Contribute](contribute/testing-guide.md) |

The content on this site comes directly from [the repository's docs/ directory](https://github.com/voidvec/fulla/tree/master/docs) (single source of truth, zero copying); if you find an error, please open a PR against the repository documentation — the site is rebuilt automatically from master.
