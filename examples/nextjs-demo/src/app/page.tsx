// Landing page: sign-in entry point. The whole OAuth dance is server-side —
// this page only links to /api/login and explains what will happen.

import { cookies } from 'next/headers';
import { SESSION_COOKIE, decodeCookie, type DemoSession } from '@/lib/fulla';

export default async function Home({
  searchParams,
}: {
  searchParams: Promise<{ error?: string }>;
}) {
  const [session, params] = await Promise.all([cookies(), searchParams]);
  const active = decodeCookie<DemoSession>(session.get(SESSION_COOKIE)?.value);

  return (
    <main style={{ maxWidth: 640, margin: '0 auto', padding: '4rem 1.5rem' }}>
      <h1>fulla × Next.js demo</h1>
      <p style={{ color: '#4b5563' }}>
        App Router + <code>fulla-oauth2</code>: authorization-code with PKCE S256, rotating refresh, userinfo,
        protected API, RP-initiated logout.
      </p>
      {params.error ? (
        <p role="alert" style={{ padding: '0.75rem 1rem', background: '#fef2f2', border: '1px solid #fecaca', borderRadius: 8 }}>
          Login failed: <code>{params.error}</code>
        </p>
      ) : null}
      {active ? (
        <p>
          <a href="/profile">Continue to your profile →</a>
        </p>
      ) : (
        <p>
          <a
            id="login"
            href="/api/login"
            style={{
              display: 'inline-block',
              padding: '0.75rem 1.5rem',
              background: '#111827',
              color: '#fff',
              borderRadius: 8,
              textDecoration: 'none',
            }}
          >
            Sign in with Fulla
          </a>
        </p>
      )}
    </main>
  );
}
