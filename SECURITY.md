# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.x (latest release) | Yes |
| < 1.0 | No |

## Reporting a Vulnerability

Please **do not report security vulnerabilities through public GitHub
issues, discussions, or pull requests.**

Instead, use GitHub's private vulnerability reporting:
[Report a vulnerability](https://github.com/voidvec/fulla/security/advisories/new).

Include as much of the following as you can:

- Affected component (endpoint, package, config surface) and version/tag
- Reproduction steps or proof-of-concept
- Impact assessment (what an attacker gains)
- Any suggested remediation

You can expect an acknowledgement within **7 days** and a status update
within **30 days**. Please allow us reasonable time to ship a fix before
public disclosure; we will credit reporters in the release notes unless you
prefer otherwise.

## Verifying Release Artifacts

Container images are signed with cosign (keyless, GitHub OIDC) and every
release ships SPDX SBOMs plus SHA-256 checksums — verification commands are
in the [README](README.md#releases--supply-chain-security).

## Hardening Guidance

- [Security Architecture](docs/architecture/security-architecture.md) (includes the Security Operations Checklist)
- [Account Lockout](docs/operate/account-lockout.md)
