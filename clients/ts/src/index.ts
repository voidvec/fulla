/**
 * fulla-oauth2 — TypeScript/JavaScript SDK for Fulla.
 *
 * Two surfaces:
 * - `./auth` (handwritten): PKCE authorization-code flow, refresh rotation,
 *   client-credentials (M2M). Zero runtime dependencies.
 * - `./generated` (from apps/server/openapi.yaml via pinned
 *   @hey-api/openapi-ts): fully typed endpoint functions + a configurable
 *   fetch client. Regenerate with `npm run generate` / tools/clients/
 *   regen_clients.py --ts-only; the CI drift gate keeps it in sync.
 */
export * from './auth.js';
export * from './generated/index.js';
