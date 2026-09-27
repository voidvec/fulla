// fulla.dev — Docusaurus site.
// Core decision (docs/documentation-governance.md §4): the repo's docs trees
// ARE the site source (docs.path below points one level up for the default
// en locale; the zh-CN translation tree lives under website/i18n/zh-CN/).
// No content copies between locales' structure — same layout, same URLs.
// Local-only process docs live in docs-local/ (outside the trees, gitignored).
//
// i18n note: the config module is evaluated ONCE per process even when
// building all locales, so per-locale strings (navbar/footer labels, sidebar
// categories) are localized through the official translation files under
// i18n/zh-CN/ (theme-classic navbar.json/footer.json + plugin-content-docs
// current.json) — NOT through any config-level locale detection.

const lightCodeTheme = require('prism-react-renderer').themes.github;
const darkCodeTheme = require('prism-react-renderer').themes.dracula;

/** @type {import('@docusaurus/types').Config} */
const config = {
  title: 'fulla',
  tagline: 'High-performance open-source IAM core · C++17',
  url: 'https://fulla.dev',
  baseUrl: '/',
  onBrokenLinks: 'throw',
  favicon: 'img/favicon.svg',
  organizationName: 'voidvec',
  projectName: 'fulla',

  // English-primary site (governance v4): '/' serves English, '/zh-CN'
  // serves the Chinese translation, navbar carries a locale switcher.
  // Both locales build in CI; translation duty = same-PR dual writes.
  i18n: {
    defaultLocale: 'en',
    locales: ['en', 'zh-CN'],
    localeConfigs: {
      en: { label: 'English', htmlLang: 'en' },
      'zh-CN': { label: '简体中文', htmlLang: 'zh-CN' },
    },
  },

  presets: [
    [
      'classic',
      /** @type {import('@docusaurus/preset-classic').Options} */
      ({
        docs: {
          path: '../docs',
          routeBasePath: '/docs',
          sidebarPath: require.resolve('./sidebars.js'),
          editUrl: 'https://github.com/voidvec/fulla/edit/master/docs',
          editLocalizedFiles: true,
          showLastUpdateTime: true,
          exclude: [
            'README.md',
          ],
        },
        // Blog lives at the repo root (`../blog`), mirroring the docs-tree
        // single-source pattern: site content IS repo content. Posts are
        // English-primary; zh-CN translations seat at
        // website/i18n/zh-CN/docusaurus-plugin-content-blog/ (same-PR dual
        // writes when a translation exists; untranslated posts simply do not
        // appear under /zh-CN, which is the launch-post cadence: EN first,
        // ZH a week later).
        blog: {
          path: '../blog',
          routeBasePath: 'blog',
          // README.md is the directory's convention doc, not a post.
          exclude: ['README.md'],
          editUrl: 'https://github.com/voidvec/fulla/edit/master',
          showReadingTime: true,
          postsPerPage: 8,
          blogSidebarCount: 'ALL',
          blogTitle: 'Fulla Blog',
          blogDescription: 'Engineering notes on building a C++17 IAM core — benchmarks, OAuth2/OIDC internals, and the road to production.',
          feedOptions: {
            type: ['rss', 'atom'],
            title: 'Fulla Blog',
            description: 'Engineering notes on building a C++17 IAM core',
            copyright: 'Copyright © 2026 Luca · AGPL-3.0',
            language: 'en',
          },
        },
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      }),
    ],
  ],

  plugins: [
    // Offline local search (Chinese via nodejieba + English, no Algolia).
    [
      require.resolve('@cmfcmf/docusaurus-search-local'),
      {
        indexDocs: true,
        indexBlog: true,
        indexPages: true,
        indexDocSidebarParentCategories: 2,
        language: ['en', 'zh'],
      },
    ],
  ],

  markdown: {
    mermaid: true,
  },
  themes: ['@docusaurus/theme-mermaid'],

  themeConfig:
    /** @type {import('@docusaurus/preset-classic').ThemeConfig} */
    ({
      colorMode: {
        defaultMode: 'light',
        disableSwitch: false,
        respectPrefersColorScheme: true,
      },
      navbar: {
        title: 'fulla',
        logo: { alt: 'fulla', src: 'img/favicon.svg' },
        items: [
          { to: '/docs/intro', label: 'Docs', position: 'left' },
          { to: '/docs/domains/api-reference', label: 'API', position: 'left' },
          { to: '/blog', label: 'Blog', position: 'left' },
          {
            href: 'https://github.com/voidvec/fulla/blob/master/benchmarks/competitors/results/COMPARISON.md',
            label: 'Benchmarks',
            position: 'left',
          },
          { to: '/docs/adr/ADR-0001', label: 'ADR', position: 'left' },
          {
            type: 'localeDropdown',
            position: 'right',
          },
          {
            href: 'https://github.com/voidvec/fulla',
            label: 'GitHub',
            position: 'right',
          },
        ],
      },
      footer: {
        style: 'dark',
        links: [
          {
            title: 'Docs',
            items: [
              { label: 'Get Started', to: '/docs/intro' },
              { label: 'Architecture', to: '/docs/architecture/architecture-overview' },
              { label: 'API Reference', to: '/docs/domains/api-reference' },
              { label: 'SDK Integration', to: '/docs/sdk/sdk-integration-guide' },
            ],
          },
          {
            title: 'Resources',
            items: [
              { label: 'Production Deployment', to: '/docs/operate/deployment' },
              { label: 'Benchmarks', href: 'https://github.com/voidvec/fulla/blob/master/benchmarks/competitors/results/COMPARISON.md' },
              { label: 'OpenAPI Contract', href: 'https://github.com/voidvec/fulla/blob/master/apps/server/openapi.yaml' },
              { label: 'ADRs', to: '/docs/adr/ADR-0001' },
              { label: 'Commercial Licensing', to: '/commercial' },
            ],
          },
          {
            title: 'Community',
            items: [
              { label: 'GitHub', href: 'https://github.com/voidvec/fulla' },
              { label: 'Chinese Wiki', href: 'https://github.com/voidvec/fulla/wiki' },
              { label: 'Issues', href: 'https://github.com/voidvec/fulla/issues' },
            ],
          },
        ],
          copyright: 'Copyright © 2026 Luca · AGPL-3.0 · Site content from the repo’s docs tree (single source of truth)',
      },
      prism: {
        theme: lightCodeTheme,
        darkTheme: darkCodeTheme,
        additionalLanguages: ['docker', 'powershell'],
      },
      tableOfContents: {
        minHeadingLevel: 2,
        maxHeadingLevel: 4,
      },
    }),
};

module.exports = config;
