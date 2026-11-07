# Fulla Go Client

Typed Go SDK for [Fulla](https://github.com/voidvec/fulla) — the embeddable C++ OAuth2/OIDC
authorization server.

- **Module path:** `github.com/voidvec/fulla/clients/go` (monorepo subdirectory module)
- **API surface:** generated from the server's single-source OpenAPI spec (`apps/server/openapi.yaml`)
  with [oapi-codegen](https://github.com/oapi-codegen/oapi-codegen) v2.8.0 — 78 operations, typed
  request/response models
- **Auth layer:** handwritten (`auth/` package) — token lifecycle is too sensitive to template

```bash
go get github.com/voidvec/fulla/clients/go
```

> The Go module proxy serves subdirectory modules from nested git tags (`clients/go/vX.Y.Z`) which the
> release pipeline creates alongside the root release tag — available since v1.3.0.

## Quickstart: machine-to-machine (client_credentials)

```go
package main

import (
	"context"

	af "github.com/voidvec/fulla/clients/go/auth"
)

func main() {
	ctx := context.Background()
	client, err := af.NewM2MClient(ctx, "http://localhost:5555", "backend-svc", "…", []string{"tokens:read"})
	if err != nil {
		panic(err)
	}
	// Every request carries a valid Bearer token: fetched lazily, cached,
	// refreshed ahead of expiry, shared across goroutines (x/oauth2
	// clientcredentials -- not reinvented).
	resp, err := client.GetWellKnownOpenidConfigurationWithResponse(ctx)
	_ = resp
	_ = err
}
```

One-shot token fetch (scripts, benchmarks):

```go
token, err := af.FetchClientCredentialsToken(ctx, "http://localhost:5555", "backend-svc", "…", []string{"tokens:read"})
```

## Introspect / revoke (client Basic authentication)

`/oauth2/introspect` and `/oauth2/revoke` authenticate the *calling client*, not a user bearer token
(RFC 7662 §2.1). Confidential clients must use HTTP Basic — the server rejects credentials in the
body (F-017):

```go
client, _ := af.NewBasicAuthClient("http://localhost:5555", "backend-svc", "…")
form := url.Values{"token": {accessToken}}
resp, err := client.PostOauth2IntrospectWithBody(ctx, "application/x-www-form-urlencoded",
	strings.NewReader(form.Encode()))
```

## Authorization-code flow (web apps, with PKCE)

```go
flow := af.NewAuthCodeFlow("http://localhost:5555", "my-client", "my-secret",
	"https://my.app/callback", []string{"openid", "profile"})

pkce, _ := af.NewPkcePair()
authorizeURL, _ := flow.BuildAuthorizeURL(sessionCSRF, &pkce)
// → redirect the user's browser to authorizeURL; Fulla sends them back
//   with ?code=…&state=… — VERIFY state, then:
tokens, err := flow.ExchangeCode(ctx, code, pkce.Verifier)
// … later; Fulla rotates refresh tokens on every use (V008):
tokens, err = flow.Refresh(ctx, tokens.RefreshToken)
```

## Generated API package

Everything under `generated/` is the typed surface of the whole server API
(`generated.ClientWithResponses` offers per-endpoint typed responses):

```go
import "github.com/voidvec/fulla/clients/go/generated"

client, _ := generated.NewClientWithResponses("http://localhost:5555")
resp, _ := client.GetWellKnownOpenidConfigurationWithResponse(ctx)
fmt.Println(resp.JSON200.Issuer)
```

## Development

```bash
cd clients/go
go test ./...                  # unit tests (httptest, no server needed)
go test -tags integration ./...  # needs FULLA_BASE_URL + a running stack
```

Regenerating the committed `generated/client.gen.go` after an `openapi.yaml` change:

```bash
python tools/clients/regen_clients.py   # from the repo root
```

CI (`.github/workflows/clients-sdk.yml`) re-generates and diffs on every PR touching `clients/**`
or the spec — committed generated code can never go stale.

## Versioning

Go modules in subdirectories resolve from nested tags (`clients/go/vX.Y.Z`) created by the release
pipeline. The module path carries no `/vN` suffix, so it serves v0/v1 versions; a future v2 of the
server API will move the module to `.../clients/go/v2` (Go module semantic import versioning).

## Local network note

If proxy.golang.org is unreachable from your network, set
`GOPROXY=https://goproxy.cn,direct` (or another mirror) for module downloads. CI runners are
unaffected.
