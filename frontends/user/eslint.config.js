import js from '@eslint/js'
import tseslint from 'typescript-eslint'
import pluginVue from 'eslint-plugin-vue'
import globals from 'globals'

// #108: lint gate for the user portal. Flat config, non-type-checked preset
// (type coverage is the build's job: tsc --noEmit runs in `npm run build`).
// vue3-recommended rules come from eslint-plugin-vue; adjust here, not by
// sprinkling eslint-disable.
export default tseslint.config(
  { ignores: ['dist/**', 'coverage/**', 'playwright-report/**', 'test-results/**', 'scratch/**'] },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  ...pluginVue.configs['flat/recommended'],
  {
    files: ['**/*.vue'],
    languageOptions: {
      parserOptions: { parser: tseslint.parser },
    },
  },
  // App sources run in the browser: DOM + WebAuthn globals (ESLint's no-undef
  // does not read tsconfig lib.dom). public/*.js counts too: boot.js is the
  // pre-paint script served as-is (no bundler), but it still runs in the page.
  {
    files: ['src/**/*.ts', 'src/**/*.vue', 'public/**/*.js'],
    languageOptions: { globals: globals.browser },
  },
  // Node-context files (vite/playwright configs, scratch scripts, e2e specs
  // running under Playwright's node): Node globals, CommonJS allowed.
  {
    files: ['*.config.ts', '*.config.js', '*.cjs', 'tests/**/*.ts'],
    languageOptions: { globals: globals.node },
    rules: { '@typescript-eslint/no-require-imports': 'off' },
  },
  {
    rules: {
      // vue3-recommended defaults tightened for this codebase:
      'vue/multi-word-component-names': 'off', // route views are single-word by convention (LoginPage, etc.)
      '@typescript-eslint/no-unused-vars': ['error', { argsIgnorePattern: '^_', varsIgnorePattern: '^_' }],
      'no-empty': ['error', { allowEmptyCatch: true }], // empty catch = deliberate fire-and-forget
      '@typescript-eslint/no-explicit-any': 'warn', // ratchet down after the .vue any-sweep
    },
  },
)
