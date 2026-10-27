// GET /api/logout — OIDC RP-Initiated Logout: hand fulla the id_token_hint,
// clear the local session, and let the server show its logged-out page
// before redirecting back to the demo root (FULLA_POST_LOGOUT_URI).

import { NextRequest, NextResponse } from 'next/server';
import { SESSION_COOKIE, POST_LOGOUT_URI, decodeCookie, flow, type DemoSession } from '@/lib/fulla';

export async function GET(request: NextRequest): Promise<NextResponse> {
  const session = decodeCookie<DemoSession>(request.cookies.get(SESSION_COOKIE)?.value);
  const response = NextResponse.redirect(
    session?.id_token
      ? flow().buildEndSessionUrl({ idTokenHint: session.id_token, postLogoutRedirectUri: POST_LOGOUT_URI, state: 'demo-logout' })
      : new URL('/', request.nextUrl.origin),
  );
  response.cookies.delete(SESSION_COOKIE);
  return response;
}
