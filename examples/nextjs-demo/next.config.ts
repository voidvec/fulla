import path from 'node:path';
import type { NextConfig } from 'next';

// fulla-oauth2 arrives as a `file:` dependency — a SYMLINK to
// ../../clients/ts, which lives outside this demo's folder. Turbopack
// refuses modules outside its root unless the root is raised; point it at
// the repo root so the SDK resolves exactly like a normal dependency.
// (webpack needs no help; keeping both paths working future-proofs the
// `--webpack` escape hatch.)
const nextConfig: NextConfig = {
  turbopack: {
    root: path.join(__dirname, '../../'),
  },
};

export default nextConfig;
