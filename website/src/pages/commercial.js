import React from 'react';
import clsx from 'clsx';
import Layout from '@theme/Layout';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';

import styles from './commercial.module.css';

// Commercial contact email. Leave empty to hide the email CTA (GitHub stays
// the primary channel); fill in one line here and the mailto button renders.
const CONTACT_EMAIL = '';

const REPO_URL = 'https://github.com/voidvec/fulla';
const ISSUE_URL = 'https://github.com/voidvec/fulla/issues/new?labels=commercial';
const MAINTAINER_URL = 'https://github.com/voidvec';
const LICENSE_URL = 'https://github.com/voidvec/fulla/blob/master/LICENSE';
const CLA_URL = 'https://github.com/voidvec/fulla/blob/master/CLA.md';

const STRINGS = {
  en: {
    title: 'fulla — Commercial Licensing',
    description:
      'fulla is open source under AGPL-3.0. Commercial licenses for closed-source integration and OEM redistribution; enterprise subscriptions (directory sync, policy management, audit compliance — planned). Login protocols stay free forever.',
    heroTitle: 'Commercial Licensing',
    heroSubtitle:
      'fulla is an open-source IAM core under AGPL-3.0. Everything you need to run a complete identity system is — and stays — free. A commercial license covers non-AGPL distribution terms; enterprise operational capabilities (directory sync, policy management, audit compliance) are planned.',
    heroCtaIssue: 'Open an issue (commercial label)',
    heroCtaEmail: 'Email us',
    commitTitle: 'The open-core commitment',
    commitSubtitle: 'Three commitments that come before any pricing discussion.',
    commitCards: [
      {
        title: 'Protocol semantics are free forever',
        body: 'The complete OAuth2/OIDC surface — including organization claims, organization-level admin consent, org-scoped tokens, and federated login via OIDC-in (next on the core roadmap) — is part of the open-source core, delivered under AGPL-3.0. It will never move behind a paywall.',
      },
      {
        title: 'Core never moves down',
        body: 'What is in the free core today stays in the free core. Paid tiers are added on top — operational automation, compliance, support — not carved out of what you already run.',
      },
      {
        title: 'No SSO tax',
        body: 'Login and authorization protocols are never something you pay for. The paid tier covers operations — directory sync, policy management, audit compliance, support — not the protocols your users authenticate with.',
      },
    ],
    optionsTitle: 'Licensing options',
    optionsSubtitle: 'One codebase: AGPL-3.0 by default, commercial terms when your distribution model needs them.',
    optionCards: [
      {
        tag: 'Free',
        title: 'Open-source core (AGPL-3.0)',
        body: 'The complete IAM: protocol engine, admin console, user frontend, multi-tenancy, SDKs. Use it, fork it, run it in production — under AGPL-3.0 (LICENSE).',
        linkLabel: 'Read the LICENSE',
        linkUrl: LICENSE_URL,
      },
      {
        tag: 'Per agreement',
        title: 'Commercial license',
        body: 'Embedding or redistributing fulla inside a closed-source product? AGPL-3.0 may not fit your distribution model. A commercial license grants the distribution terms AGPL does not; the licensing framework is anchored in the repository (LICENSE + CLA.md).',
        linkLabel: 'View the CLA',
        linkUrl: CLA_URL,
      },
      {
        tag: 'Planned',
        title: 'Enterprise subscription',
        body: 'Self-hosted, annual, tiered by organization size. Unlocks the enterprise capabilities below — plus a license-key entitlement binding updates and support to your subscription.',
      },
    ],
    pricingTitle: 'Pricing structure',
    pricingSubtitle:
      'Illustrative structure; quotes are per deployment and available on request.',
    tiers: [
      { name: 'Small', band: 'Up to ~500 organization members' },
      { name: 'Medium', band: 'Up to ~5,000 organization members' },
      { name: 'Large', band: 'Unlimited organization members' },
    ],
    tierQuote: 'Quote on request',
    pricingPoints: [
      'Annual subscription — one predictable line item per year.',
      'Tiered by organization size, not by per-user license counts.',
      'No MAU / monthly-active-user billing — the norm for self-hosted IAM; your costs do not swing with login traffic.',
      'One price list: every enterprise capability (and the reserved agent-governance slot) sits in the single Enterprise tier — no edition matrix.',
    ],
    entTitle: 'Enterprise capabilities',
    entSubtitle:
      'Customer-triggered roadmap: planned, delivered with customer engagement — none of these are available today.',
    entItems: [
      {
        title: 'License-key entitlement',
        body: 'The subscription proof: an offline-friendly signed key that binds update releases and support to your subscription. An entitlement, not DRM — the source stays in the AGPL repository.',
      },
      {
        title: 'SCIM 2.0 directory sync',
        body: 'Automated user and group provisioning from your IdP (Entra ID, Okta) into fulla organizations. Federated login itself stays in the free core — SCIM is directory operations, not a login protocol.',
      },
      {
        title: 'Organization policy pack',
        body: 'Password policies, session lifetimes, lockout thresholds, and mandatory WebAuthn — enforced per organization, overriding the global defaults.',
      },
      {
        title: 'Audit retention & compliance reporting',
        body: 'Organization-sliced compliance reports — logins, administrative actions, authorization changes — with configurable retention and export.',
      },
      {
        title: 'Agent governance (reserved slot)',
        body: 'A reserved slot on the same price list: when AI agents need governed, auditable access through fulla, the entitlement lands here — not in a separate edition.',
      },
    ],
    contactTitle: 'Talk to us',
    contactText:
      'The fastest channel is GitHub: open an issue with the commercial label, or reach the maintainer directly.',
    contactIssue: 'Open an issue (commercial label)',
    contactProfile: 'Maintainer profile',
    legalNote:
      'The pricing structure above is illustrative and nothing on this page is a binding offer; final terms are set in a written agreement.',
  },
  'zh-CN': {
    title: 'fulla — 商业授权',
    description:
      'fulla 以 AGPL-3.0 开源。为闭源集成/OEM 再分发提供商业许可；企业订阅（目录同步、策略管理、审计合规——规划中）。登录协议永久免费。',
    heroTitle: '商业授权',
    heroSubtitle:
      'fulla 是 AGPL-3.0 开源的 IAM 内核。运行一套完整身份系统所需的一切，现在且永远免费。商业许可覆盖非 AGPL 分发条款；企业运营能力（目录同步、策略管理、审计合规）为规划项。',
    heroCtaIssue: '提 issue（commercial 标签）',
    heroCtaEmail: '邮件联系',
    commitTitle: '开源核心承诺',
    commitSubtitle: '在任何定价讨论之前，先说清楚什么永远不收费。',
    commitCards: [
      {
        title: '协议语义永久免费',
        body: '完整的 OAuth2/OIDC 协议面——包括组织 claims、组织级管理员授权（admin consent）、组织作用域令牌，以及 OIDC-in 联邦登录（core 路线图下一站）——属于开源核心，以 AGPL-3.0 交付，永远不会移到付费墙之后。',
      },
      {
        title: 'Core 功能永不下放',
        body: '今天在免费 core 里的功能，就一直留在免费 core 里。付费层只在其上叠加——运营自动化、合规、支持——不会从你已在运行的部件中挖走。',
      },
      {
        title: '不收 SSO 税',
        body: '登录与授权协议永远不收费。付费层覆盖的是运营——目录同步、策略管理、审计合规、支持——而不是你的用户用来登录的协议。',
      },
    ],
    optionsTitle: '授权选项',
    optionsSubtitle: '同一套代码：默认 AGPL-3.0；当你的分发模式需要时，提供商业条款。',
    optionCards: [
      {
        tag: '免费',
        title: '开源核心（AGPL-3.0）',
        body: '完整 IAM：协议引擎、管理后台、用户前端、多租户、SDK。按 AGPL-3.0 使用、fork、投入生产（见 LICENSE）。',
        linkLabel: '阅读 LICENSE',
        linkUrl: LICENSE_URL,
      },
      {
        tag: '按合同约定',
        title: '商业许可',
        body: '要把 fulla 嵌入或再分发进闭源产品？AGPL-3.0 可能不适合你的分发模式。商业许可授予 AGPL 不授予的分发条款；许可框架锚定在仓库内（LICENSE + CLA.md）。',
        linkLabel: '查看 CLA',
        linkUrl: CLA_URL,
      },
      {
        tag: '规划中',
        title: '企业订阅',
        body: '自托管、按年订阅、按组织规模分档。解锁下方全部企业能力——外加把更新与支持绑定到订阅的 license-key 凭证。',
      },
    ],
    pricingTitle: '定价结构',
    pricingSubtitle: '示意结构；报价按部署单独评估，联系我们获取。',
    tiers: [
      { name: '小型', band: '组织成员 ~500 以内' },
      { name: '中型', band: '组织成员 ~5,000 以内' },
      { name: '大型', band: '组织成员数不限' },
    ],
    tierQuote: '报价联系获取',
    pricingPoints: [
      '年订阅——每年一笔可预期的支出。',
      '按组织规模分档，不按用户数逐个计费。',
      '无 MAU / 月活计费——这是自托管 IAM 的行业惯例；你的成本不随登录量波动。',
      '一张价目表：全部企业能力（含 agent 治理预留位）都在同一个 Enterprise 档——不做版本矩阵。',
    ],
    entTitle: '企业能力',
    entSubtitle: '客户触发式路线图：规划项，随客户协作交付——以下当前均不可用。',
    entItems: [
      {
        title: 'License-key 订阅凭证',
        body: '订阅证明：一把离线友好的签名 key，把更新发布与支持绑定到你的订阅。这是权益凭证而非 DRM——源码留在 AGPL 仓库内。',
      },
      {
        title: 'SCIM 2.0 目录同步',
        body: '从你的 IdP（Entra ID、Okta）向 fulla 组织自动供给用户与组。联邦登录本身留在免费 core——SCIM 是目录运营，不是登录协议。',
      },
      {
        title: '组织策略包',
        body: '密码策略、会话时长、锁定阈值、强制 WebAuthn——按组织生效，覆盖全局默认值。',
      },
      {
        title: '审计留存与合规报告',
        body: '按组织切片的合规报告——登录、管理操作、授权变更——支持留存策略配置与导出。',
      },
      {
        title: 'Agent 治理（预留位）',
        body: '同一张价目表上的预留位：当 AI agent 需要经由 fulla 的受治理、可审计访问时，权益落在这里——不另立版本。',
      },
    ],
    contactTitle: '联系我们',
    contactText: '最快的通道是 GitHub：打上 commercial 标签提一个 issue，或直接联系维护者。',
    contactIssue: '提 issue（commercial 标签）',
    contactProfile: '维护者主页',
    legalNote: '上述定价结构仅为示意，本页内容不构成有约束力的要约；最终条款以书面协议为准。',
  },
};

const CARD_ICONS = [
  // shield-check (protocol semantics)
  <svg key="ps" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8">
    <path d="M12 3l8 4v5c0 5-3.5 8-8 9-4.5-1-8-4-8-9V7l8-4z" strokeLinejoin="round" />
    <path d="M9 12l2 2 4-4" strokeLinecap="round" strokeLinejoin="round" />
  </svg>,
  // layers (core never moves down)
  <svg key="ly" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8">
    <path d="M12 3l9 5-9 5-9-5 9-5z" strokeLinejoin="round" />
    <path d="M3 13l9 5 9-5" strokeLinecap="round" strokeLinejoin="round" />
    <path d="M3 17.5l9 5 9-5" strokeLinecap="round" strokeLinejoin="round" />
  </svg>,
  // lock-open (no SSO tax)
  <svg key="lo" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8">
    <rect x="4" y="11" width="16" height="9" rx="2" />
    <path d="M8 11V7a4 4 0 017.66-1.6" strokeLinecap="round" />
  </svg>,
];

export default function Commercial() {
  const { i18n } = useDocusaurusContext();
  const t = STRINGS[i18n.currentLocale] || STRINGS.en;

  return (
    <Layout title={t.title} description={t.description}>
      <main>
        {/* ---------------- Page header ---------------- */}
        <section className={styles.pageHero}>
          <div className="container">
            <h1>{t.heroTitle}</h1>
            <p className={styles.pageHeroSubtitle}>{t.heroSubtitle}</p>
            <div className={styles.ctaRow}>
              <a className="button button--primary button--lg" href={ISSUE_URL} target="_blank" rel="noopener">
                {t.heroCtaIssue}
              </a>
              {CONTACT_EMAIL ? (
                <a className="button button--secondary button--lg" href={`mailto:${CONTACT_EMAIL}`}>
                  {t.heroCtaEmail}
                </a>
              ) : null}
            </div>
          </div>
        </section>

        {/* ---------------- Open-core commitment ---------------- */}
        <section className={clsx(styles.section, styles.commitmentSection)}>
          <div className="container">
            <h2 className={styles.sectionTitle}>{t.commitTitle}</h2>
            <p className={styles.sectionSubtitle}>{t.commitSubtitle}</p>
            <div className={styles.cardRow}>
              {t.commitCards.map((c, i) => (
                <div key={c.title} className={styles.card}>
                  <div className={styles.cardIcon}>{CARD_ICONS[i]}</div>
                  <h3>{c.title}</h3>
                  <p>{c.body}</p>
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* ---------------- Licensing options ---------------- */}
        <section className={styles.section}>
          <div className="container">
            <h2 className={styles.sectionTitle}>{t.optionsTitle}</h2>
            <p className={styles.sectionSubtitle}>{t.optionsSubtitle}</p>
            <div className={styles.cardRow}>
              {t.optionCards.map((c) => (
                <div key={c.title} className={styles.card}>
                  <span className={styles.cardTag}>{c.tag}</span>
                  <h3>{c.title}</h3>
                  <p>{c.body}</p>
                  {c.linkUrl ? (
                    <a className={styles.cardLink} href={c.linkUrl} target="_blank" rel="noopener">
                      {c.linkLabel} →
                    </a>
                  ) : null}
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* ---------------- Pricing structure ---------------- */}
        <section className={clsx(styles.section, styles.altSection)}>
          <div className="container">
            <h2 className={styles.sectionTitle}>{t.pricingTitle}</h2>
            <p className={styles.sectionSubtitle}>{t.pricingSubtitle}</p>
            <div className={styles.cardRow}>
              {t.tiers.map((tier) => (
                <div key={tier.name} className={clsx(styles.card, styles.tierCard)}>
                  <div className={styles.tierName}>{tier.name}</div>
                  <div className={styles.tierBand}>{tier.band}</div>
                  <div className={styles.tierQuote}>{t.tierQuote}</div>
                </div>
              ))}
            </div>
            <ul className={styles.pricingPoints}>
              {t.pricingPoints.map((p) => (
                <li key={p}>{p}</li>
              ))}
            </ul>
          </div>
        </section>

        {/* ---------------- Enterprise capabilities ---------------- */}
        <section className={styles.section}>
          <div className="container">
            <h2 className={styles.sectionTitle}>{t.entTitle}</h2>
            <p className={styles.sectionSubtitle}>{t.entSubtitle}</p>
            <div className={styles.entGrid}>
              {t.entItems.map((item) => (
                <div key={item.title} className={styles.entCard}>
                  <h3>{item.title}</h3>
                  <p>{item.body}</p>
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* ---------------- Contact ---------------- */}
        <section className={clsx(styles.section, styles.closing)}>
          <div className="container">
            <h2>{t.contactTitle}</h2>
            <p>{t.contactText}</p>
            <div className={styles.ctaRow}>
              <a className="button button--primary button--lg" href={ISSUE_URL} target="_blank" rel="noopener">
                {t.contactIssue}
              </a>
              <a className="button button--secondary button--lg" href={MAINTAINER_URL} target="_blank" rel="noopener">
                {t.contactProfile}
              </a>
              {CONTACT_EMAIL ? (
                <a className="button button--secondary button--lg" href={`mailto:${CONTACT_EMAIL}`}>
                  {t.heroCtaEmail}
                </a>
              ) : null}
            </div>
            <p className={styles.legalNote}>{t.legalNote}</p>
            <p className={styles.repoNote}>
              <a href={REPO_URL} target="_blank" rel="noopener">
                {REPO_URL.replace('https://', '')}
              </a>
            </p>
          </div>
        </section>
      </main>
    </Layout>
  );
}
