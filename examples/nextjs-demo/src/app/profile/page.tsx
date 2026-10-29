// /profile — the protected page. Reads the session cookie server-side and
// calls userinfo with the Bearer. When the access token has expired, the
// refresh leg (which MUST run in a route handler — server components
// cannot persist cookies) happens in /api/refresh, which bounces back here.

import { cookies } from 'next/headers';
import { redirect } from 'next/navigation';
import { SESSION_COOKIE, decodeCookie, fetchUserInfo, type DemoSession } from '@/lib/fulla';

export default async function ProfilePage() {
  const store = await cookies();
  const session = decodeCookie<DemoSession>(store.get(SESSION_COOKIE)?.value);
  if (!session) {
    redirect('/');
  }

  const userinfo = await fetchUserInfo(session.access_token);
  if (!userinfo) {
    redirect(session.refresh_token ? '/api/refresh?next=%2Fprofile' : '/?error=session_expired');
  }

  return (
    <main style={{ maxWidth: 640, margin: '0 auto', padding: '4rem 1.5rem' }}>
      <h1>Your profile</h1>
      <p data-testid="signed-in">
        Signed in as <strong>{String(userinfo['name'] ?? userinfo['preferred_username'] ?? userinfo['sub'])}</strong>
      </p>
      <h2>userinfo (scope-gated claims)</h2>
      <pre style={{ background: '#fff', border: '1px solid #e5e7eb', borderRadius: 8, padding: '1rem', overflowX: 'auto' }}>
        {JSON.stringify(userinfo, null, 2)}
      </pre>
      <p>
        <a href="/api/me" data-testid="api-me-link">
          See the same identity served by this app&rsquo;s protected API →
        </a>
      </p>
      <p>
        <a id="logout" href="/api/logout" style={{ color: '#b91c1c' }}>
          End session
        </a>
      </p>
    </main>
  );
}
