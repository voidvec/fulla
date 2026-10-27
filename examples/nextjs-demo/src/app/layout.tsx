import type { Metadata } from 'next';
import type { ReactNode } from 'react';

export const metadata: Metadata = {
  title: 'fulla × Next.js demo',
  description: 'Authorization-code + PKCE login through the fulla-oauth2 SDK',
};

export default function RootLayout({ children }: { children: ReactNode }) {
  return (
    <html lang="en">
      <body
        style={{
          margin: 0,
          fontFamily: 'system-ui, -apple-system, sans-serif',
          background: '#f6f7f9',
          color: '#111827',
          minHeight: '100vh',
        }}
      >
        {children}
      </body>
    </html>
  );
}
