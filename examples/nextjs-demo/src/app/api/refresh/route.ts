// GET /api/refresh?next=%2Fprofile — rotate the refresh token and persist
// the new session cookie. `next` is whitelisted to this demo's own pages
// (never an absolute URL): a redirect target that echoes user input is an
// open-redirect sink.

import { NextRequest, NextResponse } from 'next/server';
import { SESSION_COOKIE, decodeCookie, encodeCookie, flow, type DemoSession } from '@/lib/fulla';

// Only same-app path destinations; keys are the accepted ?next= values.
const ALLOWED_NEXT: Record<string, string> = {
  '/profile': '/profile',
  '/': '/',
};

export async function GET(request: NextRequest): Promise<NextResponse> {
  const next = ALLOWED_NEXT[request.nextUrl.searchParams.get('next') ?? ''] ?? '/profile';
  const session = decodeCookie<DemoSession>(request.cookies.get(SESSION_COOKIE)?.value);
  if (!session?.refresh_token) {
    const expired = NextResponse.redirect(new URL('/?error=session_expired', request.nextUrl.origin));
    expired.cookies.delete(SESSION_COOKIE);
    return expired;
  }
  try {
    // Fulla rotates on every use (migration V008): the OLD refresh token is
    // single-use — persisting the response is not optional.
    const tokens = await flow().refresh(session.refresh_token);
    const refreshed = NextResponse.redirect(new URL(next, request.nextUrl.origin));
    refreshed.cookies.set(
      SESSION_COOKIE,
      encodeCookie({
        access_token: tokens.access_token,
        refresh_token: tokens.refresh_token ?? session.refresh_token,
        id_token: tokens.id_token ?? session.id_token,
        expires_at: Math.floor(Date.now() / 1000) + (tokens.expires_in ?? 3600),
      }),
      { httpOnly: true, sameSite: 'lax', path: '/', maxAge: 86400 },
    );
    return refreshed;
  } catch (error) {
    console.error('[demo] refresh failed:', error);
    const failed = NextResponse.redirect(new URL('/?error=refresh_failed', request.nextUrl.origin));
    failed.cookies.delete(SESSION_COOKIE);
    return failed;
  }
}
