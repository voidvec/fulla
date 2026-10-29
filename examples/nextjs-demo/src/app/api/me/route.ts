// GET /api/me — the demo's "protected API": a Bearer-authenticated call to
// fulla's userinfo using the access token from the session cookie. Same
// shape a real backend route would take from an Authorization header.

import { NextRequest, NextResponse } from 'next/server';
import { SESSION_COOKIE, decodeCookie, fetchUserInfo, type DemoSession } from '@/lib/fulla';

export async function GET(request: NextRequest): Promise<NextResponse> {
  const session = decodeCookie<DemoSession>(request.cookies.get(SESSION_COOKIE)?.value);
  if (!session) {
    return NextResponse.json({ error: 'unauthenticated' }, { status: 401 });
  }
  const userinfo = await fetchUserInfo(session.access_token);
  if (!userinfo) {
    return NextResponse.json({ error: 'token_expired' }, { status: 401 });
  }
  return NextResponse.json({ source: 'protected-api', ...userinfo });
}
