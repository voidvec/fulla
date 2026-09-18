#!/usr/bin/env node
// scripts/gen-llms-txt.mjs
//
// Wave 0 (AI-native DX): generates llms.txt (index) and llms-full.txt
// (full-text concatenation) into website/static/ so Docusaurus publishes
// them at the site root (https://fulla.dev/llms.txt). Spec convention:
// https://llmstxt.org
//
// Regenerate + commit whenever docs/ changes; CI (website workflow) fails
// the build when the committed files are stale:
//   node scripts/gen-llms-txt.mjs && git diff --exit-code website/static/llms*.txt
//
// Design constraints:
// - Zero npm dependencies (Node >= 20 built-ins only) so any environment
//   (including CI) can run it without an install step.
// - Deterministic output: fixed category order + sorted files within a
//   category, so the freshness gate never flakes on ordering.
// - URL mapping mirrors website/docusaurus.config.js: docs live at
//   ../docs with routeBasePath '/docs' and no sidebar_custom_slug anywhere,
//   so docs/<rel>.md -> https://fulla.dev/docs/<rel-without-.md>.

import { readFile, writeFile } from 'node:fs/promises';
import { readdir, stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const docsDir = path.join(repoRoot, 'docs');
const outDir = path.join(repoRoot, 'website', 'static');
const siteBase = 'https://fulla.dev';

// Reading order follows the site IA (website/sidebars.js): evaluate first,
// deep reference last. Entries the order list misses are appended under
// 'Reference' (sorted) so a new doc can never be silently dropped.
const categoryOrder = [
  { title: 'Start', match: (rel) => rel === 'intro.md' },
  { title: 'Guides', match: (rel) => rel.startsWith('guides/') },
  { title: 'Evaluate', match: (rel) => rel.startsWith('architecture/') || rel.startsWith('benchmark/') },
  { title: 'Domains', match: (rel) => rel.startsWith('domains/') },
  { title: 'SDK', match: (rel) => rel.startsWith('sdk/') },
  { title: 'Operate', match: (rel) => rel.startsWith('operate/') },
  { title: 'Contribute', match: (rel) => rel.startsWith('contribute/') },
  { title: 'Decisions', match: (rel) => rel.startsWith('adr/') },
];

const MAX_SUMMARY = 160;
const MAX_FULL_BYTES = 1024 * 1024; // 1 MiB cap for llms-full.txt

async function walkMarkdown(dir, prefix = '') {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      files.push(...(await walkMarkdown(path.join(dir, entry.name), rel)));
    } else if (entry.name.endsWith('.md')) {
      files.push(rel);
    }
  }
  return files;
}

function stripFrontmatter(text) {
  // CRLF-tolerant: repo files mix LF/CRLF, and a missed strip leaks
  // `sidebar_position: N` frontmatter into titles/summaries.
  const m = text.match(/^---\r?\n[\s\S]*?\r?\n---\r?\n/);
  return m ? text.slice(m[0].length) : text;
}

function parseDoc(rel, raw) {
  const text = stripFrontmatter(raw);
  const lines = text.split('\n');
  const h1 = (lines.find((l) => l.startsWith('# ')) || `# ${rel}`).slice(2).trim();
  let summary = '';
  let inCode = false;
  for (const line of lines.slice(1)) {
    if (line.trim().startsWith('```')) inCode = !inCode;
    if (inCode) continue;
    const t = line.trim();
    if (!t || t.startsWith('#') || t.startsWith('|') || t.startsWith('---') || t.startsWith('>'))
      continue;
    summary = t.replace(/\[([^\]]+)\]\([^)]*\)/g, '$1');
    break;
  }
  if (summary.length > MAX_SUMMARY) summary = summary.slice(0, MAX_SUMMARY - 1) + '…';
  return { rel, url: `${siteBase}/docs/${rel.replace(/\.md$/, '')}`, title: h1, summary, text };
}

async function main() {
  const rels = (await walkMarkdown(docsDir)).filter((r) => r !== 'README.md');
  const docs = [];
  for (const rel of rels) {
    docs.push(parseDoc(rel, await readFile(path.join(docsDir, rel), 'utf8')));
  }

  const buckets = categoryOrder.map((c) => ({ ...c, docs: [] }));
  const reference = { title: 'Reference', docs: [] };
  for (const doc of docs) {
    const bucket = buckets.find((b) => b.match(doc.rel));
    (bucket ?? reference).docs.push(doc);
  }
  for (const bucket of [...buckets, reference]) bucket.docs.sort((a, b) => a.rel.localeCompare(b.rel));

  // ---- llms.txt (index) ----
  let index = `# fulla\n\n> fulla is a self-hosted OAuth 2.1 / OpenID Connect identity provider written in C++ (Drogon): authorization-code + PKCE, client credentials, device flow, WebAuthn/MFA, organizations, and an open application platform — deployable as a single Docker image backed by PostgreSQL.\n\n`;
  let first = true;
  for (const bucket of [...buckets, reference]) {
    if (bucket.docs.length === 0) continue;
    index += `${first ? 'Essential' : bucket.title} docs:\n`;
    for (const doc of bucket.docs) index += `- [${doc.title}](${doc.url}): ${doc.summary}\n`;
    index += '\n';
    first = false;
  }
  index += `Full documentation (all pages concatenated): ${siteBase}/llms-full.txt\n`;
  await writeFile(path.join(outDir, 'llms.txt'), index, 'utf8');

  // ---- llms-full.txt (concatenation, capped) ----
  let full = `# fulla — full documentation\n\n> All English documentation pages concatenated for LLM consumption. Source of truth: https://fulla.dev/docs\n\n`;
  let bytes = Buffer.byteLength(full, 'utf8');
  for (const bucket of [...buckets, reference]) {
    for (const doc of bucket.docs) {
      const section = `\n\n---\n\n# ${doc.title}\n\nSource: ${doc.url}\n\n${doc.text.trim()}\n`;
      const size = Buffer.byteLength(section, 'utf8');
      if (bytes + size > MAX_FULL_BYTES) {
        console.warn(`llms-full.txt capped at ${MAX_FULL_BYTES} bytes; dropping ${doc.rel} and the rest of its category.`);
        continue; // keep scanning: smaller later sections may still fit
      }
      full += section;
      bytes += size;
    }
  }
  await writeFile(path.join(outDir, 'llms-full.txt'), full, 'utf8');

  console.log(`llms.txt: ${docs.length} docs indexed; llms-full.txt: ${(bytes / 1024).toFixed(0)} KiB.`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
