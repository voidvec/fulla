import { defineConfig } from '@playwright/test';

// The interop runner (interop.yml) starts `next start` on 8080 itself and
// passes DEMO_BASE_URL/FULLA_* explicitly; locally `npm run dev` matches
// the defaults.
export default defineConfig({
  testDir: './e2e',
  timeout: 30_000,
  retries: 0,
  reporter: process.env.CI ? 'github' : 'list',
  use: {
    baseURL: process.env.DEMO_BASE_URL ?? 'http://localhost:8080',
  },
});
