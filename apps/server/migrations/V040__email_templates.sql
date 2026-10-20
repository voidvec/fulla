-- V040 (P1-2, #278): email template store.
--
-- Production emails (verification / password reset / org invitation) were
-- hardcoded C++ string concatenation. This table holds editable, localized
-- templates (subject + HTML body + plain-text fallback body). Seeded with
-- defaults that are semantically equivalent to the previous hardcoded copy.
--
-- Security posture (design deliverables/p1-fix-design-2026-10.md §P1-2):
--   * save-time static validation lives in the application layer
--     (EmailTemplateService::validateTemplateContent) -- no <script>/<iframe>/
--     <form>, outbound href domains restricted to email.allowed_link_domains;
--   * verification/reset link domains come ONLY from global config
--     (config.frontend.url) at send time -- never from org-controlled input;
--   * seeds are idempotent (ON CONFLICT DO NOTHING) so db-reset replays and
--     existing deployments both converge on the same defaults.

CREATE TABLE IF NOT EXISTS email_templates (
    id SERIAL PRIMARY KEY,
    template_key VARCHAR(64) NOT NULL,
    locale VARCHAR(8) NOT NULL DEFAULT 'en',
    subject TEXT NOT NULL,
    body_html TEXT NOT NULL,
    body_text TEXT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT uq_email_templates UNIQUE (template_key, locale)
);

-- Seed defaults (3 keys x 2 locales; each row carries both text and HTML).
-- Variable placeholders use {{name}} syntax; the engine HTML-escapes every
-- substituted value. The only link variables are {{verify_link}},
-- {{reset_link}} (global-config-derived domains) and {{invite_token}}.
-- body_text values use E'' strings (\n = real newline); multi-line values
-- concatenate with || (PostgreSQL has no adjacent-literal concatenation).

INSERT INTO email_templates (template_key, locale, subject, body_html, body_text) VALUES
-- email_verification / en
('email_verification', 'en', 'Verify Your Email',
 '<p>Please verify your email by clicking the link below:</p>'
  || '<p><a href="{{verify_link}}">Verify your email</a></p>'
  || '<p>This link expires in 24 hours. If you did not create an account, you can ignore this email.</p>',
 E'Please verify your email by clicking:\n\n{{verify_link}}\n\nThis link expires in 24 hours.'),
-- email_verification / zh-CN
('email_verification', 'zh-CN', '验证您的邮箱',
 '<p>请点击下方链接验证您的邮箱：</p>'
  || '<p><a href="{{verify_link}}">验证邮箱</a></p>'
  || '<p>链接有效期 24 小时。如果您没有注册账号，请忽略本邮件。</p>',
 E'请点击以下链接验证您的邮箱：\n\n{{verify_link}}\n\n本链接有效期 24 小时。'),
-- password_reset / en
('password_reset', 'en', 'Password Reset Request',
 '<p>We received a request to reset your password.</p>'
  || '<p><a href="{{reset_link}}">Reset your password</a></p>'
  || '<p>This link expires in 15 minutes. If you did not request a reset, you can ignore this email.</p>',
 E'Click the following link to reset your password:\n\n{{reset_link}}\n\nThis link expires in 15 minutes.'),
-- password_reset / zh-CN
('password_reset', 'zh-CN', '重置密码请求',
 '<p>我们收到了重置您密码的请求。</p>'
  || '<p><a href="{{reset_link}}">重置密码</a></p>'
  || '<p>链接有效期 15 分钟。如果这不是您本人的操作，请忽略本邮件。</p>',
 E'请点击以下链接重置您的密码：\n\n{{reset_link}}\n\n本链接有效期 15 分钟。'),
-- org_invitation / en
('org_invitation', 'en', 'Fulla Organization Invitation',
 '<p>You have been invited to join the organization <strong>{{org_name}}</strong> on Fulla.</p>'
  || '<p>Invitation token (valid for 72 hours, single use):</p>'
  || '<p><code>{{invite_token}}</code></p>'
  || '<p>Sign in to the portal, open My Organizations, and paste the token under "Accept an invitation". '
  || 'If you did not expect this invitation you can ignore this email.</p>',
 E'You have been invited to join the organization "{{org_name}}" on Fulla.\n\n'
  || E'Invitation token (valid for 72 hours, single use):\n  {{invite_token}}\n\n'
  || E'Sign in to the portal, open My Organizations, and paste the token under "Accept an invitation". '
  || E'If you did not expect this invitation you can ignore this email.'),
-- org_invitation / zh-CN
('org_invitation', 'zh-CN', 'Fulla 组织邀请',
 '<p>您被邀请加入 Fulla 上的组织 <strong>{{org_name}}</strong>。</p>'
  || '<p>邀请令牌（有效期 72 小时，仅可使用一次）：</p>'
  || '<p><code>{{invite_token}}</code></p>'
  || '<p>请登录门户，打开“我的组织”，在“接受邀请”处粘贴该令牌。如果您没有预期收到此邀请，请忽略本邮件。</p>',
 E'您被邀请加入 Fulla 上的组织“{{org_name}}”。\n\n'
  || E'邀请令牌（有效期 72 小时，仅可使用一次）：\n  {{invite_token}}\n\n'
  || E'请登录门户，打开“我的组织”，在“接受邀请”处粘贴该令牌。如果您没有预期收到此邀请，请忽略本邮件。')
ON CONFLICT (template_key, locale) DO NOTHING;
