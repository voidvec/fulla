#!/usr/bin/env node
// scripts/check-zh-mirror-consistency.mjs
//
// Structural parity gate between the EN docs tree (docs/, canonical) and the
// zh-CN mirror (website/i18n/zh-CN/docusaurus-plugin-content-docs/current/).
// Translation quality is out of scope — this catches the drift class the
// 2026-11 documentation survey found (error-code table 30 vs 44 rows, whole
// sections missing while CI stayed green). Failures:
//   * H2 section count mismatch (sections dropped or not yet translated)
//   * api-reference error-code table row-count mismatch (sync-points.md hard
//     rule; ErrorCatalogDocTest only validates the EN table)
//   * zh orphan (mirror file with no EN counterpart)
//   * EN file with no mirror — allowlisted below (each entry must carry the
//     reason and is expected to shrink to zero)
// Version-string drift is a WARNING: legitimate translations occasionally
// cite different versions, so it reports without failing.

import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const enDir = path.join(repoRoot, 'docs');
const zhDir = path.join(
  repoRoot, 'website', 'i18n', 'zh-CN',
  'docusaurus-plugin-content-docs', 'current');

// EN pages deliberately without a zh mirror (yet). Keep this list SHORT and
// dated — every entry is translation debt; the goal is always zero.
// (2026-11-08: emptied — ADR-0014's zh translation landed; 50 EN / 50 zh.)
const MIRROR_ALLOWLIST = new Map([
]);

async function walkMarkdown(dir, prefix = '') {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) files.push(...(await walkMarkdown(path.join(dir, entry.name), rel)));
    else if (entry.name.endsWith('.md') && entry.name !== 'README.md') files.push(rel);
  }
  return files;
}

function stripFrontmatter(text) {
  const m = text.match(/^---\r?\n[\s\S]*?\r?\n---\r?\n/);
  return m ? text.slice(m[0].length) : text;
}

function h2Count(text) {
  let n = 0, inCode = false;
  for (const line of text.split('\n')) {
    if (line.trim().startsWith('```')) inCode = !inCode;
    if (!inCode && /^##\s/.test(line)) n++;
  }
  return n;
}

function errorTableRows(text) {
  // Rows like: | `VALIDATION_USERNAME_TAKEN` | 3006 | VALIDATION | 409 | …
  return (text.match(/^\| `[A-Z][A-Z0-9_]*` \| \d+ \|/gm) || []).length;
}

function versionTokens(text) {
  // Code-free version mentions (rough: strip fenced blocks first).
  const noCode = text.replace(/```[\s\S]*?```/g, '');
  return new Set(noCode.match(/\bv\d+\.\d+\.\d+\b/g) || []);
}

const [enRels, zhRels] = [new Set(await walkMarkdown(enDir)), new Set(await walkMarkdown(zhDir))];
const failures = [];
const warnings = [];

for (const rel of zhRels) {
  if (!enRels.has(rel)) failures.push(`zh orphan: ${rel} has no EN counterpart`);
}

for (const rel of enRels) {
  if (!zhRels.has(rel)) {
    const reason = MIRROR_ALLOWLIST.get(rel);
    if (reason) warnings.push(`no zh mirror (allowlisted): ${rel} — ${reason}`);
    else failures.push(`no zh mirror: ${rel}`);
    continue;
  }
  const en = stripFrontmatter(await readFile(path.join(enDir, rel), 'utf8'));
  const zh = stripFrontmatter(await readFile(path.join(zhDir, rel), 'utf8'));

  const enH2 = h2Count(en), zhH2 = h2Count(zh);
  if (enH2 !== zhH2) failures.push(`H2 mismatch: ${rel} — EN ${enH2} vs zh ${zhH2}`);

  if (rel === 'domains/api-reference.md') {
    const enRows = errorTableRows(en), zhRows = errorTableRows(zh);
    if (enRows !== zhRows)
      failures.push(`error-code table mismatch: ${rel} — EN ${enRows} vs zh ${zhRows} rows (sync-points hard rule)`);
  }

  const enV = versionTokens(en), zhV = versionTokens(zh);
  const onlyEn = [...enV].filter((v) => !zhV.has(v));
  const onlyZh = [...zhV].filter((v) => !enV.has(v));
  if (onlyEn.length || onlyZh.length)
    warnings.push(`version drift: ${rel} — only-EN ${onlyEn.join(',') || '—'} / only-zh ${onlyZh.join(',') || '—'}`);
}

for (const w of warnings) console.warn(`WARN  ${w}`);
if (failures.length > 0) {
  for (const f of failures) console.error(`FAIL  ${f}`);
  console.error(`zh mirror consistency: ${failures.length} failure(s), ${warnings.length} warning(s)`);
  process.exit(1);
}
console.log(`zh mirror consistency: OK (${enRels.size} EN / ${zhRels.size} zh files, ${warnings.length} warning(s))`);
