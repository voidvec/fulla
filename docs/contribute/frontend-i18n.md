# Frontend i18n Guide — Selection, Scope, and Contributor Workflow

> Decision record: [ADR-0013](../adr/ADR-0013.md). This guide carries the evaluation evidence, the impact inventory, the adoption plan with acceptance criteria, and the day-to-day contributor workflow. The zh-CN mirror of this page is maintained in the same PR (docs dual-write convention).

## TL;DR

- Both frontends speak **English (default) and Simplified Chinese** through **vue-i18n v11** message catalogs — one switcher drives page chrome *and* error messages.
- **translate.js and other runtime machine-translation widgets are rejected** for the product UI: they ship page text to a third-party cloud, translate after first paint, and cannot guarantee OAuth terminology. Machine translation may assist translators offline, never the runtime.
- Adding a language = one new catalog file + registration (see [Contributor workflow](#contributor-workflow)).

## 1. What was evaluated

### 1.1 translate.js (xnx3/translate) — automatic page translation

| Dimension | Finding |
|---|---|
| License / health | MIT; ~3.1k stars; last release v4.0.0 (2026-02), repo active — **embedding would be license-clean** |
| Mechanism | DOM walker post-load; batches text nodes to the author's cloud (`api.translate.zvo.cn`, `america.api…`); swaps translations back into the page |
| Privacy | **Page text egress to a third-party service by default.** For an IdP whose pages render usernames, emails, and error detail, this is disqualifying |
| Capacity / availability | Free channel has a **daily character cap**; runtime depends on external nodes; paid "private deployment" exists but is a commercial dependency |
| UX | Translates *after* first paint (FOUC by design); re-translation on SPA route changes relies on a mutation listener with open Vue issues (#54, #94); rewrites `input` value attributes — hazard on login/MFA forms |
| Quality | No terminology control: "consent", "scope", "authorization code" get whatever the engine guesses; untranslated/over-translated mix is common |
| Size | ~47 KB gzip runtime (vs ~10–14 KB brotli for vue-i18n) |

**Verdict: technically embeddable, rejected as the fulla UI mechanism.** The failure modes (data egress, offline breakage, terminology drift) land exactly where an identity provider cannot afford them.

### 1.2 Curated-catalog libraries for Vue 3

| Option | Fit for fulla | Notes |
|---|---|---|
| **vue-i18n v11 (intlify)** — *selected* | ★★★★★ | De-facto Vue standard (MIT, ~3.9M dl/wk). Composition API + `globalInjection` means templates use `$t()` with no per-component boilerplate; plain TS catalog modules stay type-checked; no extra build plugin needed at our catalog size |
| i18next + i18next-vue | ★★★☆☆ | Superb ecosystem, but Vue binding is the side door (~96k dl/wk); two-layer architecture is overkill here |
| LinguiJS v6 | ★★☆☆☆ | ICU + compile-time extraction, but **no official Vue runtime** — core-only integration |
| Paraglide JS 2 (inlang) | ★★★★☆ | Most modern (tree-shaken compiled messages, full type safety); younger ecosystem, `setLocale` reloads by default — revisit if catalog size ever matters |
| FormatJS / vue-intl | ★★☆☆☆ | Full ICU/CLDR but Vue binding ~5k dl/wk, ICU `}}` collides inside Vue templates |
| typesafe-i18n | ★★★☆☆ | Tiny runtime, fully typed; repo transferred, slow maintenance; own message format |

Selection logic: the error catalog (`services/messages/`) is *already* a per-locale registry keyed by code — vue-i18n is the same idea for UI chrome, with the largest ecosystem and hiring pool.

### 1.3 Locale matrix

| Tier | Locales | Status |
|---|---|---|
| 1 (shipped) | `en` (default), `zh-CN` | Full catalogs (user app 188 leaf keys, admin 306), switcher, tests — matches the docs-site convention (`defaultLocale: 'en'` + `zh-CN` mirror) |
| 2 (roadmap, on demand) | `zh-TW`, `ja`, `de`, `es`, `fr`, `pt-BR`, `ru` | The set that "makes the cut" in comparable OSS IdPs (Keycloak's most-complete translations); add per [workflow](#contributor-workflow) when a community need exists |

## 2. Impact inventory (what i18n touches)

### 2.1 Applications and routes

**`frontends/user` (user portal)** — 15 routes:

| Route | View | Purpose |
|---|---|---|
| `/login` | `pages/auth/LoginPage.vue` | Login + MFA, social buttons |
| `/register` | `pages/auth/RegisterPage.vue` | Account creation |
| `/forgot-password` | `pages/auth/ForgotPasswordPage.vue` | Reset request |
| `/reset-password` | `pages/auth/ResetPasswordPage.vue` | Consume reset token |
| `/verify-email` | `pages/auth/VerifyEmailPage.vue` | Email verification result |
| `/callback` | `pages/oauth/CallbackPage.vue` | Code exchange; maps protocol error codes via the shared catalog |
| `/callback/{github,google,wechat}` | `pages/oauth/{GitHub,Social}CallbackPage.vue` | Social login/link callbacks |
| `/consent` | `pages/oauth/ConsentPage.vue` | OAuth consent screen + scope descriptions |
| `/`, `/profile`, `/security`, `/authorized-apps` | `pages/account/*.vue` | Account area (inside `AppLayout`) |
| — | `layouts/AppLayout.vue`, `layouts/AuthLayout.vue` | Nav, user menu, theme toggle, footer |

**`frontends/admin` (admin console)** — 12 routes under `components/layout/AdminLayout.vue`: `LoginPage`, dashboard, applications (list + detail tabs), users (list + detail), roles, scopes, audit logs, tokens, device approval, settings.

### 2.2 Where strings lived before extraction

1. **Template literals** — hardcoded English in both apps, plus four admin spots in Chinese (grant-type descriptions ×2 files, one validation message, one Client-Credentials scope note).
2. **Script constants** — nav items, scope-description maps, grant-type option arrays, inline validation messages.
3. **Error catalog** — `services/messages/zh-CN.ts` (~48 codes incl. reserved `__unknown__`/`__network__`, RFC 6749/7009/8628/6750 + OIDC codes), previously the only Chinese surface, now bilingual.
4. **ARIA labels / attributes** — theme-toggle labels, alert dismiss, modal close; these are user-visible to assistive tech and are translated.

### 2.3 Cross-app invariants that constrain the work

- `components/ui/*.vue` are **byte-identical across apps**, enforced by `scripts/check-ui-sync.mjs` in CI — shared components (`LocaleSwitcher.vue` included) are edited in both copies. Templates use `$t()` only (no script imports), which keeps sync trivial.
- `services/errorAdapter.ts` + `services/messages/` are mirrored files (canonical: user app). The mirror is enforced by the **error catalog section of `scripts/check-ui-sync.mjs`**: every entry of the user catalog must be in the admin catalog with the identical literal, in each locale (admin may carry additional admin-only codes). No frontend test can check this, because test files never import across apps — `crossAppConsistency.property.test.ts` only establishes that the shared source is deterministic. The mirrored files stay **dependency-free** (no vue-i18n, no DOM): the active locale travels through `services/locale.ts`, a plain module the i18n bootstrap pushes into.
- Property 13 (`messageCatalog.property.test.ts`) requires **every backend Error_Code and protocol code** to have a clean, non-empty entry — in **every registered locale**.
- E2E suites run pinned to `en-US` (`playwright.config.ts` `use.locale`); Chinese paths are covered by a dedicated `i18n.spec.ts` per app.

### 2.4 Out of scope (non-goals)

- Backend response messages (frontends map error **codes** locally; `/callback` maps the protocol `error` code through the catalog and only shows a raw `error_description` as a secondary detail line when the server sent one — that string is backend-owned).
- The docs website (already dual-locale via Docusaurus).
- `<title>` (brand names: "Fulla" / "Fulla Admin"), number/date formatting (`Intl` when a need appears).

## 3. Architecture

```
src/i18n/
  index.ts     # createI18n(legacy:false, globalInjection:true), detection, setLocale(), initI18n()
  en.ts        # UI catalog (namespaces: common/ui/nav[/auth/oauth/account | admin…])
  zh-CN.ts     # mirror catalog
  i18nKeys.test.ts  # key parity + call-site key existence (mechanical gate)
src/services/
  locale.ts    # zero-dependency active-locale state (node-safe; unit-test friendly)
  messages/    # error catalog: en.ts + zh-CN.ts; getErrorMessage(code) defaults to the ACTIVE locale
```

- **Detection & persistence** (mirrors the `fulla-theme` store pattern): `localStorage['fulla-locale']` → `navigator.languages` best-match (`zh*` → `zh-CN`, else `en`) → `en`. `initI18n()` runs **synchronously before `app.mount`** — no language flash — and the inline script in `index.html` pre-syncs `<html lang>` for screen readers.
- **One switcher drives everything (fully reactive since #158)**: `setLocale()` updates the composer locale, persists, and syncs `document.documentElement.lang`. The services layer stays dependency-free: `src/i18n/index.ts` injects a locale getter (`setLocaleGetter(() => i18n.global.locale.value)`), so `getErrorMessage(code)` called inside a render effect tracks the vue-i18n locale ref. Error state therefore stores the **`NormalizedError`, not the resolved string**, and pages/banners re-resolve the message at render time (`errorText` computed) — an already-surfaced error re-translates when the locale switches. Parameterized chrome copy passed as plain strings (`t('...', {param})`) keeps snapshot semantics by design; the e2e suite covers both behaviors.
- **Usage**: templates `$t('auth.login.title')` (works in the byte-synced `components/ui` too); `<script setup>` `const { t } = useI18n()` — **no options** (global scope); plain `.ts` modules `i18n.global.t(...)`. Under `legacy: false`, `i18n.global.locale` is a ref — read `.value` in scripts; templates unwrap automatically. Reactive constants (nav items, option arrays) are `computed`.
- **Errors**: `getErrorMessage(code)` defaults to the active UI locale; `DEFAULT_LOCALE = 'en'` is only the table fallback for an unregistered locale.
- **Fonts**: both apps load Noto Sans SC so Chinese never falls back to a system font.
- **Build plugin (`@intlify/unplugin-vue-i18n`, #159)**: the UI catalogs (`src/i18n/*.ts`, plain `export default` objects) are AOT-precompiled at build time and production builds bundle vue-i18n **runtime-only** — the message compiler is not shipped (~90-100 KB smaller main chunk) and per-key first-`t()` compilation cost is gone. Dev server and vitest keep the full build, so tests exercise the same message resolution. The `include` list in `vite.config.ts` names the two catalog files explicitly (never application code); the error catalog under `services/messages/` is a plain lookup table, never goes through vue-i18n, and stays out of the plugin. Two guards keep this honest: the **bundle-size budget gate** (`node scripts/check-frontend-size.mjs` after builds, wired into `_frontend.yml`) fails when total or entry-chunk JS exceeds the recorded baseline +10% — the compiler re-entering the bundle (~+90 KB main) trips it mechanically; and `vite preview` + both locales is the one-off smoke that proved the precompiled path (repeat it manually when touching the i18n build wiring).

## 4. Adoption plan and acceptance (executed)

| Phase | Work | Acceptance criteria | Result |
|---|---|---|---|
| 1. Infrastructure | `vue-i18n@11` in both apps; `src/i18n/` + `services/locale.ts`; `services/messages/en.ts` + registry change; `LocaleSwitcher.vue` mounted (user: header + auth footer; admin: topbar + login card); Noto Sans SC in admin | build green ×2; switch persists + syncs `<html lang>`; `check-ui-sync.mjs` green | ✅ |
| 2. Extraction | All views/layouts/components translated (en verbatim except 4 fixed admin spots); constants → `computed` | tsc + build + lint green ×2; CJK leftover audit clean; key-parity green | ✅ 188 + 306 leaf keys |
| 3. Tests | Property 13 → all locales; errorAdapter property → en default + zh explicit; `i18nKeys.test.ts` per app; e2e zh→en; `i18n.spec.ts` ×2; `use.locale: 'en-US'` pinned | unit + e2e + lint green ×2 | ✅ user 34 unit / 118 e2e; admin 4 unit / 185 e2e + 6 env skips |
| 4. Docs | This guide + ADR-0013, en + zh-CN mirrors, sidebar | docs build green; dual-write complete | ✅ |
| 5. Full acceptance | Full frontend suite both apps | lint, build, unit/property, e2e green ×2, zero new skips | ✅ |

**Overall acceptance (semantic)**: a default English session shows zero frontend-owned Chinese strings on every route, and a 简体中文 session shows zero frontend-owned English strings — enforced mechanically by the CJK audit (`grep -rn "[一-鿿]" src --include=*.vue --include=*.ts | grep -v "src/i18n/" | grep -v "src/services/messages/"` returns nothing) and by `i18n.spec.ts`. Named exceptions: backend-provided strings (e.g. a raw `error_description` detail line, success `message` fields), native endonyms in the locale menu, and the `'中文'` short label inside the byte-synced switcher.

## 5. Contributor workflow

**Add a string**: pick/extend a namespace (`auth.login.title`), add the key to **both** `en.ts` and `zh-CN.ts` in the same PR, use `$t()`/`t()` at the call site. The `i18nKeys.test.ts` gate fails the build on en/zh key drift, unknown keys at call sites, and `ui.*` divergence between the two apps. Interpolation via named params (`{name}` appears in both locales); never concatenate translated fragments.

**Error-catalog authoring constraints (Property 13 tripwires — never loosen the patterns)**: message text must not contain `{{`, `}}`, `${`, `%s`, `%d`, or the English words *exception*, *traceback*, or *stack trace* (single-brace `{name}` interpolation is fine). These run against **every** registered locale.

**Add a locale** (tier-2): create `src/i18n/<locale>.ts` (translate from `en.ts`; machine-assisted translation is fine **with human review** — that is the only permitted use of MT), register it in `SUPPORTED_LOCALES` + `createI18n` + a switcher label, add `services/messages/<locale>.ts` mirroring every code, extend the parity test's locale list, and check CJK font coverage. Property 13 + the parity tests enforce completeness automatically.

**Key conventions**: namespace = area (`common`, `ui`, `nav`, plus app-specific `auth`/`oauth`/`account` in user, `admin.<page>` in admin); `ui.*` keys (used by byte-synced `components/ui/*`) must resolve identically in **both** apps — enforced by the user app's `i18nKeys.test.ts` cross-check.
