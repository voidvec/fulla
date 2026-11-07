# Support

Thanks for using Fulla! This page routes you to the right channel.

| I want to… | Channel |
|---|---|
| Ask a question / discuss usage or design | [GitHub Discussions](https://github.com/voidvec/fulla/discussions) |
| Report a bug or request a feature | [open an issue](https://github.com/voidvec/fulla/issues/new/choose) (bug report or feature request template) |
| Report a security vulnerability | **privately** — see [SECURITY.md](SECURITY.md); do not open a public issue |
| Read the docs | [fulla.dev](https://fulla.dev) (rendered site) or the [`docs/`](docs/) tree in this repo |
| Use the Python / Go / TypeScript client SDKs | [`clients/python`](clients/python) / [`clients/go`](clients/go) / [`clients/ts`](clients/ts) (npm `fulla-oauth2`, since v1.6.0) READMEs + the [API reference](https://fulla.dev) — install issues go to GitHub issues with the `clients` label |
| Embed the C++ SDK | [SDK integration guide](docs/sdk/) in the docs tree |
| Contribute code | [CONTRIBUTING.md](CONTRIBUTING.md) |

## Before opening an issue

1. Search [existing issues](https://github.com/voidvec/fulla/issues?q=) — the
   fastest answer is often already there.
2. Check the version you run (`fulla-server --version` / image tag) and include
   it, plus the smallest reproducer you can manage.
3. For install/deploy problems, include your deployment shape (Docker Compose,
   Helm, bare binary) and relevant config (with secrets redacted).

## Response expectations

Fulla is maintained by a small team (see
[MAINTAINERS.md](MAINTAINERS.md)). There is no SLA; issues and discussions are
triaged as maintainer time allows. Security reports follow the timeline in
[SECURITY.md](SECURITY.md) and take priority.

## Commercial licensing & support

The open-source core is licensed under AGPL-3.0 — see [LICENSE](LICENSE) and
[CLA.md](CLA.md). A commercial license is available for closed-source
integration or OEM redistribution that AGPL-3.0 does not permit; a paid
enterprise subscription (directory sync, policy management, audit compliance,
support) is planned but not yet available. See
[Commercial Licensing](https://fulla.dev/commercial) for details, or open an
issue with the `commercial` label.
