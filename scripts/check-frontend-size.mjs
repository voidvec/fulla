#!/usr/bin/env node
// Bundle-size budget gate (#159): fails when raw JS output grows beyond the
// recorded baseline + 10% headroom — both the total across dist/assets/*.js
// and the index (entry) chunk of each frontend.
//
// The entry-chunk budget doubles as the AOT guard: the vue-i18n runtime
// message compiler coming back into the bundle costs ~90-100 KB on the main
// chunk, far beyond the headroom, so a plugin/alias regression trips this
// gate loudly instead of shipping a silently slower app.
//
// Run from the repo root AFTER `npm run build` in both frontends (CI does
// this in _frontend.yml). Raise a baseline ONLY with written justification
// in the PR — same discipline as the coverage ratchet.
import { readdirSync, statSync, existsSync } from 'node:fs'
import { join, resolve } from 'node:path'

// Raw-bytes baselines recorded 2026-09-08 with @intlify/unplugin-vue-i18n
// precompilation active (issue #159; see docs/contribute/frontend-i18n.md).
// Re-baselined 2026-09-17 for v1.4.0: two new lazy-loaded portal pages
// (My Applications, My Organizations), the editable profile form, bilingual
// catalogs for both, and two nav entries (PR #218). Re-measured 2026-09-18
// after the review fixes (admin governance UI + orgs page; CI-build numbers
// — the local build measured ~9KB lower).
const BUDGETS = {
  user: { total: 385441, main: 146739 },
  admin: { total: 407951, main: 146452 },
}
const HEADROOM = 1.1

let failed = false
for (const [app, budget] of Object.entries(BUDGETS)) {
  const assets = resolve(`frontends/${app}/dist/assets`)
  if (!existsSync(assets)) {
    console.error(`FAIL ${app}: ${assets} missing — run "npm run build" first`)
    failed = true
    continue
  }
  const js = readdirSync(assets).filter((f) => f.endsWith('.js'))
  const entries = js.filter((f) => /^index-.+\.js$/.test(f))
  if (entries.length !== 1) {
    console.error(`FAIL ${app}: expected exactly one index-*.js entry chunk, found ${entries.length}`)
    failed = true
    continue
  }
  const total = js.reduce((s, f) => s + statSync(join(assets, f)).size, 0)
  const main = statSync(join(assets, entries[0])).size
  const capTotal = Math.ceil(budget.total * HEADROOM)
  const capMain = Math.ceil(budget.main * HEADROOM)
  const ok = total <= capTotal && main <= capMain
  console.log(
    `${ok ? 'OK' : 'FAIL'} ${app}: total ${kb(total)}/${kb(capTotal)} (base ${kb(budget.total)}), ` +
    `main ${kb(main)}/${kb(capMain)} (base ${kb(budget.main)})`,
  )
  if (!ok) failed = true
}
process.exit(failed ? 1 : 0)

function kb(n) {
  return `${(n / 1024).toFixed(1)} KB`
}
