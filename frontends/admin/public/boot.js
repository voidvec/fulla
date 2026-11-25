// Pre-paint boot script (loaded as an external same-origin file).
//
// This logic used to live in an inline <script> in
// index.html, which the nginx CSP (`script-src 'self'`, no 'unsafe-inline')
// blocked — the "violation" console noise was the minor symptom; the real
// damage was that the anti-FOUC theme resolution and the <html lang> sync
// silently never ran. An external same-origin script is covered by 'self'
// with no CSP relaxation. MUST stay a synchronous (non-module, non-defer)
// script in <head>: its whole purpose is to run before first paint.
//
// Anti-FOUC: resolve the theme BEFORE first paint (mirrors stores/theme.ts
// read logic). Without this, dark users see a light flash while the JS bundle
// downloads.
try {
  var t = localStorage.getItem('fulla-theme')
  var dark = t === 'dark' || ((!t || t === 'system') && window.matchMedia('(prefers-color-scheme: dark)').matches)
  document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light')
} catch { /* keep default presentation */ }
// Sync <html lang> from the resolved UI locale before paint — mirrors
// i18n/index.ts detection (stored value wins; otherwise any zh browser
// language resolves zh-CN) so screen readers never see 'en' first.
try {
  var storedLocale = localStorage.getItem('fulla-locale')
  var zh
  if (storedLocale === 'zh-CN') {
    zh = true
  } else if (storedLocale !== 'en') {
    var langs = navigator.languages || [navigator.language]
    zh = langs.some(function (l) { return /^zh/i.test(l || '') })
  }
  if (zh) document.documentElement.lang = 'zh-CN'
} catch { /* keep default 'en' */ }
