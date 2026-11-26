#!/usr/bin/env node
// Dual-copy sync checker (stage 1 of the shared-UI strategy). Two sections,
// one exit status. Exit 0 = in sync, 1 = drift or a check that could not be
// carried out (CI-gateable; wired into .github/workflows/_frontend.yml).
//
//   1. UI kit: compares frontends/{admin,user}/src/components/ui recursively
//      so the two copies cannot drift. Stage 2 replaces this section when the
//      kit moves to frontends/packages/ui.
//   2. Error catalog: frontends/admin/src/services/messages/{en,zh-CN}.ts
//      are a mirror of the user catalogs plus a block of admin-only codes
//      (Requirement 8.6 / 10.7). Every entry the user app ships must be in
//      the admin app with the identical literal. This lives here and not in
//      a vitest property because test files must never import across apps
//      (.claude/rules/frontend-contract.md), so no frontend test can see
//      both catalogs; crossAppConsistency.property.test.ts only checks
//      determinism and completeness of the canonical user catalog.

import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs'
import { join, dirname, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const dirs = {
  admin: join(root, 'frontends', 'admin', 'src', 'components', 'ui'),
  user: join(root, 'frontends', 'user', 'src', 'components', 'ui'),
}

function walkVue(dir, prefix = '') {
  // Recursive: future component subdirectories must not silently escape.
  if (!existsSync(dir)) return null
  const out = []
  for (const entry of readdirSync(dir).sort()) {
    const full = join(dir, entry)
    if (statSync(full).isDirectory()) {
      out.push(...walkVue(full, `${prefix}${entry}/`))
    } else if (entry.endsWith('.vue')) {
      out.push({ rel: `${prefix}${entry}`, full })
    }
  }
  return out
}

const files = {}
for (const [port, dir] of Object.entries(dirs)) {
  const walked = walkVue(dir)
  if (walked === null) {
    console.error(`RESULT: ${port} components/ui directory is missing — cannot verify sync.`)
    process.exit(1)
  }
  if (walked.length === 0) {
    // An empty directory would trivially "match" an empty peer — that is a
    // broken state, not a pass.
    console.error(`RESULT: ${port} components/ui has no .vue files — refusing to report a vacuous pass.`)
    process.exit(1)
  }
  files[port] = walked
}

const byRel = {
  admin: new Map(files.admin.map(f => [f.rel, f])),
  user: new Map(files.user.map(f => [f.rel, f])),
}
const allRels = [...new Set([...byRel.admin.keys(), ...byRel.user.keys()])].sort()

const report = []
let drift = 0

for (const rel of allRels) {
  const a = byRel.admin.get(rel)
  const u = byRel.user.get(rel)
  if (!a) {
    report.push(`DRIFT  ${rel}  missing in admin`)
    drift++
  } else if (!u) {
    report.push(`DRIFT  ${rel}  missing in user`)
    drift++
  } else if (readFileSync(a.full).equals(readFileSync(u.full))) {
    report.push(`OK     ${rel}`)
  } else {
    report.push(`DRIFT  ${rel}  contents differ`)
    drift++
  }
}

console.log(`UI kit sync report (${relative(root, dirs.admin)} vs ${relative(root, dirs.user)})`)
console.log(`components checked: ${allRels.length}`)
console.log('')
for (const line of report) console.log('  ' + line)
console.log('')

// ---------------------------------------------------------------------------
// Section 2: error catalog mirror (user -> admin)
// ---------------------------------------------------------------------------

const catalogLocales = ['en.ts', 'zh-CN.ts']
const catalogDirs = {
  admin: join(root, 'frontends', 'admin', 'src', 'services', 'messages'),
  user: join(root, 'frontends', 'user', 'src', 'services', 'messages'),
}

// The catalogs are one entry per line: two-space indent, key, a single-
// quoted or double-quoted string literal, a comma, an optional trailing
// comment. Entries are compared as written (key plus literal), so the
// mirror holds at the source level and not only after evaluation.
const CATALOG_OBJECT_OPEN = /^export const \w+: Record<string, string> = \{$/
const CATALOG_OBJECT_CLOSE = /^}$/
const CATALOG_ENTRY = /^ {2}([A-Za-z0-9_]+): ('(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*"),[ \t]*(?:\/\/.*)?$/

/**
 * Reads one catalog file into a Map of key -> string literal.
 * Throws when the file departs from the one-entry-per-line shape. Skipping
 * a line it does not understand would quietly shrink the gate to whatever
 * is left, so an unparsable line is a failure, not a gap.
 */
function readCatalog(file) {
  const entries = new Map()
  let inside = false
  const lines = readFileSync(file, 'utf8').split(/\r?\n/)
  for (let index = 0; index < lines.length; index++) {
    const line = lines[index]
    const where = `${relative(root, file)}:${index + 1}`
    if (CATALOG_OBJECT_OPEN.test(line)) {
      inside = true
      continue
    }
    if (!inside || line === '') continue
    if (CATALOG_OBJECT_CLOSE.test(line)) {
      inside = false
      continue
    }
    if (/^\s*\/\//.test(line)) continue
    const match = CATALOG_ENTRY.exec(line)
    if (!match) {
      throw new Error(`cannot read the catalog line at ${where}: ${line.trim()}`)
    }
    if (entries.has(match[1])) {
      throw new Error(`duplicate catalog key ${match[1]} at ${where}`)
    }
    entries.set(match[1], match[2])
  }
  if (entries.size === 0) {
    throw new Error(`no catalog entries found in ${relative(root, file)}`)
  }
  return entries
}

const catalogReport = []
let catalogDrift = 0
let catalogMirrored = 0

for (const locale of catalogLocales) {
  let user
  let admin
  try {
    user = readCatalog(join(catalogDirs.user, locale))
    admin = readCatalog(join(catalogDirs.admin, locale))
  } catch (error) {
    console.error(`RESULT: error catalog mirror cannot be verified — ${error.message}`)
    process.exit(1)
  }
  for (const [key, literal] of user) {
    if (!admin.has(key)) {
      catalogReport.push(`DRIFT  ${locale}  ${key}  missing in admin`)
      catalogDrift++
    } else if (admin.get(key) !== literal) {
      catalogReport.push(`DRIFT  ${locale}  ${key}  message differs`)
      catalogDrift++
    } else {
      catalogMirrored++
    }
  }
  // Codes the admin console alone can surface (email templates, audit
  // export, claim mappings). Reported, not judged: nothing can tell an
  // intended admin-only code from one that should have been mirrored.
  const adminOnly = [...admin.keys()].filter((key) => !user.has(key))
  catalogReport.push(`INFO   ${locale}  admin-only codes: ${adminOnly.length}`)
  for (const key of adminOnly) catalogReport.push(`         ${key}`)
}

console.log(`Error catalog mirror report (${relative(root, catalogDirs.user)} -> ${relative(root, catalogDirs.admin)})`)
console.log(`entries mirrored: ${catalogMirrored}, drifted: ${catalogDrift}`)
console.log('')
for (const line of catalogReport) console.log('  ' + line)
console.log('')

if (drift > 0 || catalogDrift > 0) {
  console.log(`RESULT: ${drift} drifted component(s), ${catalogDrift} drifted catalog entry(s) — copies must stay identical.`)
  process.exit(1)
} else {
  console.log('RESULT: in sync — shared UI components are byte-identical and the user error catalog is mirrored verbatim in admin.')
}
