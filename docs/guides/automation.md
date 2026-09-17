# Automating with Fulla (no human in the loop)

Fulla speaks standard OAuth 2.0, so scripts, CLIs and service-to-service
integrations use the same protocols browsers use. This guide covers the two
supported automation paths. For letting your *users* sign in, see
[Build an app on Fulla](build-an-app.md).

## 1. Scripts and CLIs — Device Authorization Grant (device flow)

Best when a tool runs where a browser may or may not be available (a laptop
CLI, a CI job with a browser, a TV/terminal).

1. Ask an administrator (or, where enabled, self-register via Portal →
   **My Applications**) to create a client for your tool. Device flow needs
   the `urn:ietf:params:oauth:grant-type:device_code` grant type.
2. Start the flow:

```bash
curl -X POST https://your-fulla.example/oauth2/device_authorization \
  -d "client_id=YOUR_CLIENT_ID"
# -> { "device_code": "...", "user_code": "ABCD-EFGH",
#      "verification_uri": "https://your-fulla.example/oauth2/device",
#      "interval": 5, "expires_in": 600 }
```

3. Show `verification_uri` + `user_code` to the user; they approve in the
   browser.
4. Poll the token endpoint (respect `interval` and `slow_down`):

```bash
curl -X POST https://your-fulla.example/oauth2/token \
  -d "grant_type=urn:ietf:params:oauth:grant-type:device_code" \
  -d "device_code=DEVICE_CODE" \
  -d "client_id=YOUR_CLIENT_ID"
```

While pending you get `authorization_pending`; after approval you get an
`access_token` (+ `refresh_token` with the `offline_access`-style scope).

## 2. Service to service — client credentials grant

For machine-only integrations with no user context (no end-user consent, no
OIDC identity). Requires a **CONFIDENTIAL** client with
`client_credentials` in its grant types.

```bash
curl -X POST https://your-fulla.example/oauth2/token \
  -u "YOUR_CLIENT_ID:YOUR_CLIENT_SECRET" \
  -d "grant_type=client_credentials" \
  -d "scope=read"
```

Notes:

- The resulting token represents the *client*, not any user (`sub` is the
  client identity).
- Scope is bounded by the scopes registered on the client; request only what
  you need.
- Rotate secrets from the management UI (or Portal → My Applications for
  self-registered apps). Rotation invalidates the previous secret
  immediately.

## Why there are no personal API tokens

Personal long-lived API tokens would bypass scope governance, revocation and
audit paths that the standard grants already provide. Device flow +
client_credentials cover the same use cases with better security
characteristics; see the v1.4.0 design notes in
`docs-local/productization-evolution/` for the trade-off discussion.
