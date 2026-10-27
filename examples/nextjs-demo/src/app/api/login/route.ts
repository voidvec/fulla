// GET /api/login — start the authorization-code + PKCE flow: mint state and
// verifier, stash them in a 10-minute httpOnly transaction cookie, then
// redirect the browser to fulla's /oauth2/authorize.

import { NextResponse } from 'next/server';
import { PkcePair, randomToken } from 'fulla-oauth2';
import { TX_COOKIE, encodeCookie, flow } from '@/lib/fulla';

export async function GET(): Promise<NextResponse> {
  const pkce = await PkcePair.generate();
  const state = randomToken();
  const nonce = randomToken(); // rides the authorize URL; fulla echoes it in the id_token
  const url = flow().buildAuthorizeUrl({ state, nonce, pkce });
  const response = NextResponse.redirect(url);
  response.cookies.set(TX_COOKIE, encodeCookie({ state, codeVerifier: pkce.verifier }), {
    httpOnly: true,
    sameSite: 'lax',
    path: '/',
    maxAge: 600,
  });
  return response;
}
