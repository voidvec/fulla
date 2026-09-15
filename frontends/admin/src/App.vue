<template>
  <router-view />
</template>

<!-- Dark theme overrides live here, NOT in a plain .css import: the
     Tailwind v4 vite plugin strips [data-theme="dark"] custom-property
     blocks from imported CSS in dev, and layer-merges them away in
     builds when they share the tailwind root file. SFC <style> modules
     bypass that pipeline. Strategy (mockup .dark): utilities emit
     var(--color-*-N), so remapping the ramp re-skins every utility at
     once; surfaces/semantic vars use literals so they never resolve
     through a remapped step. -->
<style>
/* Native controls (select option popups, date pickers, scrollbars)
   follow color-scheme: without this the dark theme rendered them with
   browser-default light styling (issue: dark-theme dropdowns). */
html {
  color-scheme: light;
}
html[data-theme="dark"] {
  color-scheme: dark;
  /* Neutral ramp inversion (blueprint dark, hue 252 kept) */
  --color-neutral-50:  oklch(14.5% 0.005 252);
  --color-neutral-100: oklch(22% 0.006 252);
  --color-neutral-200: oklch(28% 0.005 252);
  --color-neutral-300: oklch(33% 0.006 252);
  --color-neutral-400: oklch(50% 0.007 252);
  --color-neutral-500: oklch(60% 0.008 252);
  --color-neutral-600: oklch(72% 0.008 252);
  --color-neutral-700: oklch(78% 0.006 252);
  --color-neutral-800: oklch(85% 0.005 252);
  --color-neutral-900: oklch(95% 0.006 252);

  /* Brand tint/accent steps used in surfaces & primary controls */
  --color-brand-50:  oklch(22% 0.050 252);
  --color-brand-100: oklch(26% 0.070 252);
  --color-brand-200: oklch(33% 0.080 252);
  --color-brand-600: oklch(58% 0.150 252);
  --color-brand-700: oklch(78% 0.100 252);
  /* 800/900 remapped too: text-brand-800 badges sit on brand-100, and a
     dozen hover:text-brand-800/900 links would disappear in dark if these
     kept their light-mode near-black values. */
  --color-brand-800: oklch(84% 0.085 252);
  --color-brand-900: oklch(90% 0.065 252);

  /* Semantic ramps: dark bgs/borders, bright text steps */
  --color-success-50:  oklch(24% 0.045 160);
  --color-success-100: oklch(28% 0.055 160);
  --color-success-200: oklch(35% 0.075 160);
  --color-success-700: oklch(76% 0.130 160);
  --color-warning-50:  oklch(25% 0.040 85);
  --color-warning-100: oklch(29% 0.050 85);
  --color-warning-200: oklch(36% 0.065 85);
  --color-warning-700: oklch(78% 0.120 85);
  --color-error-50:  oklch(24% 0.035 18);
  --color-error-100: oklch(28% 0.045 18);
  --color-error-200: oklch(36% 0.070 18);
  --color-error-700: oklch(74% 0.130 18);
  --color-info-50:  oklch(23% 0.030 230);
  --color-info-100: oklch(27% 0.040 230);
  --color-info-200: oklch(34% 0.060 230);
  --color-info-700: oklch(74% 0.110 230);

  /* Themeable surfaces (literals — never resolve through the ramp) */
  --color-surface: oklch(18% 0.004 252);
  --color-page:    oklch(12% 0.003 252);
  --color-ring:    oklch(63% 0.135 252 / 0.40);

  /* Semantic var layer (literals / bright refs) */
  --color-brand:          var(--color-brand-600);
  --color-brand-hover:    var(--color-brand-700);
  --color-brand-pressed:  var(--color-brand-200);
  --color-brand-subtle:   oklch(22% 0.050 252);
  --color-brand-muted:    oklch(26% 0.070 252);

  --color-text-primary:    oklch(95% 0.006 252);
  --color-text-secondary:  oklch(72% 0.008 252);
  --color-text-tertiary:   oklch(60% 0.008 252);
  --color-text-inverse:    oklch(12% 0.003 252);
  --color-text-link:       var(--color-brand-300);
  --color-text-link-hover: var(--color-brand-200);

  --color-bg-page:        oklch(12% 0.003 252);
  --color-bg-surface:     oklch(18% 0.004 252);
  --color-bg-surface-alt: oklch(22% 0.006 252);
  --color-bg-overlay:     oklch(8% 0.003 252 / 0.70);

  --color-border-default: oklch(28% 0.005 252);
  --color-border-hover:   oklch(33% 0.006 252);
  --color-border-focus:   var(--color-brand-500);

  --color-success:        oklch(76% 0.130 160);
  --color-success-bg:     oklch(24% 0.045 160);
  --color-success-border: oklch(35% 0.075 160);
  --color-warning:        oklch(78% 0.120 85);
  --color-warning-bg:     oklch(25% 0.040 85);
  --color-warning-border: oklch(36% 0.065 85);
  --color-error:          oklch(74% 0.130 18);
  --color-error-bg:       oklch(24% 0.035 18);
  --color-error-border:   oklch(36% 0.070 18);
  --color-info:           oklch(74% 0.110 230);
  --color-info-bg:        oklch(23% 0.030 230);
  --color-info-border:    oklch(34% 0.060 230);

  --shadow-xs: 0 1px 2px 0 oklch(0% 0 0 / 0.20);
  --shadow-sm: 0 1px 3px 0  oklch(0% 0 0 / 0.30),
               0 1px 2px -1px oklch(0% 0 0 / 0.25);
  --shadow-md: 0 4px 6px -1px oklch(0% 0 0 / 0.35);
  --shadow-lg: 0 10px 15px -3px oklch(0% 0 0 / 0.40);

  --ring-color: var(--color-ring);
}
</style>
