// GET /callback — the redirect_uri fulla sends the browser back to
// (registered EXACTLY as http://localhost:8080/callback for the seeded
// fulla-portal client; the server strict-matches, so the path must not
// change without registering a new client).
//
// completeAuthorizationFlow enforces the red lines before anything touches
// the network: error callbacks and state mismatches throw. On success the
// tokens land in an httpOnly session cookie and the browser ends up on
// /profile.

import { NextRequest, NextResponse } from 'next/server';
import { FullaAuthError, TokenSet } from 'fulla-oauth2';
import { SESSION_COOKIE, TX_COOKIE, decodeCookie, encodeCookie, flow, type AuthTransaction } from '@/lib/fulla';

// Redirect targets are built from the REQUEST origin only (no user input,
// no environment-derived origin) — every failure lands on this app's own
// root with an encoded reason.
function failure(origin: string, message: string): NextResponse {
  const response = NextResponse.redirect(new URL(`/?error=${encodeURIComponent(message)}`, origin));
  response.cookies.delete(TX_COOKIE);
  return response;
}

export async function GET(request: NextRequest): Promise<NextResponse> {
  const tx = decodeCookie<AuthTransaction>(request.cookies.get(TX_COOKIE)?.value);
  if (!tx) {
    // Two ways to land here without a transaction: a stale/replayed
    // callback, or the post-logout redirect (POST_LOGOUT_URI reuses the
    // registered callback family). With no `code` at all it is the latter
    // — bounce home without an error banner.
    if (!request.nextUrl.searchParams.has('code')) {
      return NextResponse.redirect(new URL('/', request.nextUrl.origin));
    }
    return failure(request.nextUrl.origin, 'missing_transaction');
  }
  const callbackUrl = request.nextUrl.href;
  let tokens: TokenSet;
  try {
    tokens = await flow().completeAuthorizationFlow(callbackUrl, {
      state: tx.state,
      codeVerifier: tx.codeVerifier,
    });
  } catch (error) {
    const message = error instanceof FullaAuthError ? error.error : 'callback_failed';
    console.error('[demo] callback failed:', error);
    return failure(request.nextUrl.origin, message);
  }
  const response = NextResponse.redirect(new URL('/profile', request.nextUrl.origin));
  response.cookies.delete(TX_COOKIE);
  response.cookies.set(
    SESSION_COOKIE,
    encodeCookie({
      access_token: tokens.access_token,
      refresh_token: tokens.refresh_token,
      id_token: tokens.id_token,
      expires_at: Math.floor(Date.now() / 1000) + (tokens.expires_in ?? 3600),
    }),
    { httpOnly: true, sameSite: 'lax', path: '/', maxAge: 86400 },
  );
  return response;
}
