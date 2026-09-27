# OAuth2 User Frontend - Test Cases

> User frontend path: `/` | Framework: Vue 3 + TailwindCSS | Playwright E2E

## Module 1: Authentication

### 1.1 Login Page (`/login`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-LOGIN-001 | Valid credentials | Enter valid username/password, click Sign In | Redirect to Dashboard (`/`) | P0 |
| U-LOGIN-002 | Empty username | Submit with empty username | HTML5 required validation prevents submit | P1 |
| U-LOGIN-003 | Empty password | Submit with empty password | HTML5 required validation prevents submit | P1 |
| U-LOGIN-004 | Wrong password | Enter valid user + wrong password | Error alert displayed | P0 |
| U-LOGIN-005 | Non-existent user | Enter unregistered username | Error message shown | P0 |
| U-LOGIN-006 | SQL injection | Enter `' OR 1=1 --` as username | Error message, no unauthorized access | P0 |
| U-LOGIN-007 | XSS in username | Enter `<script>alert('xss')</script>` | Rendered as text, no script execution | P0 |
| U-LOGIN-008 | Loading state | Submit valid credentials | Button shows loading spinner, disabled | P2 |
| U-LOGIN-009 | Redirect after login | Login with `?redirect=/profile` in URL | Redirect to `/profile` after login | P0 |
| U-LOGIN-010 | Already authenticated | Navigate to `/login` while logged in | Redirect to Dashboard | P0 |
| U-LOGIN-011 | GitHub social login | Click "Sign in with GitHub" | Redirected to GitHub OAuth page | P1 |
| U-LOGIN-012 | GitHub not offered by deployment | Discovery endpoint (`GET /api/auth/providers`) does not report github (unconfigured credentials or tier off) | GitHub button hidden, no dead link rendered | P2 |
| U-LOGIN-013 | Link to register | Click "create a new account" link | Navigate to `/register` | P1 |
| U-LOGIN-014 | Link to forgot password | Click "Forgot password?" | Navigate to `/forgot-password` | P1 |
| U-LOGIN-015 | Browser autofill | Use browser autofill for credentials | Form submits correctly with autofilled values | P2 |

### 1.2 MFA Challenge

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-MFA-001 | MFA required flow | Login with MFA-enabled user | MFA challenge form shown (6-digit input) | P0 |
| U-MFA-002 | Valid MFA code | Enter correct 6-digit TOTP code | Login succeeds, redirect to Dashboard | P0 |
| U-MFA-003 | Invalid MFA code | Enter wrong 6-digit code | Error message displayed | P0 |
| U-MFA-004 | Less than 6 digits | Enter "1234" | Submit button disabled (`mfaCode.length !== 6`) | P1 |
| U-MFA-005 | More than 6 digits | Input limited to 6 chars (maxlength=6) | Cannot enter more than 6 digits | P1 |
| U-MFA-006 | Non-numeric input | Enter letters in MFA field | Input limited by `inputmode="numeric"` | P1 |
| U-MFA-007 | Back to login | Click "Back to login" link | MFA form hidden, login form shown | P1 |
| U-MFA-008 | Loading state | Submit MFA code | Button shows loading state, disabled | P2 |
| U-MFA-009 | Expired MFA token | Wait for mfa_token to expire, then submit code | Error message, may need to re-login | P1 |

### 1.3 Registration Page (`/register`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-REG-001 | Valid registration | Fill username, email, password (6+ chars), matching confirm, submit | Success message shown, auto-redirect to login after 2s | P0 |
| U-REG-002 | Password too short | Enter 5-char password | Error: "Password must be at least 6 characters" | P0 |
| U-REG-003 | Passwords don't match | Enter different passwords | Error: "Passwords do not match" | P0 |
| U-REG-004 | Duplicate username | Register with existing username | Error message from API | P0 |
| U-REG-005 | Duplicate email | Register with existing email | Error message from API | P0 |
| U-REG-006 | Empty username | Submit with empty username | Form submits successfully (username is optional) | P1 |
| U-REG-007 | Empty email | Submit with empty email | HTML5 required validation | P1 |
| U-REG-008 | Invalid email format | Enter "not-an-email" | HTML5 email validation prevents submit | P1 |
| U-REG-009 | SQL injection in username | Enter `'; DROP TABLE users;--` | Error or registration fails safely | P0 |
| U-REG-010 | XSS in username | Enter `<script>alert(1)</script>` | Rendered as text | P0 |
| U-REG-011 | Very long username | Enter username > 255 chars | API validation error or truncation handled | P2 |
| U-REG-012 | Loading state | Submit valid form | Button shows "Creating...", disabled | P2 |
| U-REG-013 | Success redirect timing | After successful registration | Success message visible, then redirect after 2s | P2 |
| U-REG-014 | Link to login | Click "Sign in" link | Navigate to `/login` | P1 |
| U-REG-015 | Already authenticated | Navigate to `/register` while logged in | Redirect to Dashboard | P1 |

### 1.4 Forgot Password (`/forgot-password`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-FP-001 | Valid email | Enter registered email, submit | Success message shown (anti-enumeration: always success) | P0 |
| U-FP-002 | Unregistered email | Enter non-existent email | Still shows success message (anti-enumeration) | P0 |
| U-FP-003 | Invalid email format | Enter "not-email" | HTML5 email validation | P1 |
| U-FP-004 | Empty email | Submit with empty email | HTML5 required validation | P1 |
| U-FP-005 | Loading state | Submit form | Button shows "Sending...", disabled | P2 |
| U-FP-006 | Back to login link | Click "Back to Login" | Navigate to `/login` | P1 |
| U-FP-007 | API error handling | Simulate network error during submit | Still shows success (anti-enumeration by design) | P1 |

### 1.5 Reset Password (`/reset-password`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-RP-001 | Valid reset token | Navigate with valid token, enter new password | Password reset succeeds, redirect to login | P0 |
| U-RP-002 | Expired reset token | Navigate with expired token | Error: token expired or invalid | P0 |
| U-RP-003 | Invalid reset token | Navigate with random token | Error message | P0 |
| U-RP-004 | No token in URL | Navigate to `/reset-password` without token | Error or redirect to forgot-password | P1 |

### 1.6 Email Verification (`/verify-email`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-VE-001 | Valid verification token | Navigate with valid token | Email verified, success message | P0 |
| U-VE-002 | Expired token | Navigate with expired token | Error message, option to resend | P1 |
| U-VE-003 | Already verified | Verify already-verified email | Message: already verified | P1 |
| U-VE-004 | Invalid token | Navigate with random token | Error message | P0 |

---

## Module 2: OAuth2 Flows

### 2.1 Authorization Callback (`/callback`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-CB-001 | Valid authorization code | Navigate with `?code=xxx` | Code exchanged, redirect to Dashboard | P0 |
| U-CB-002 | Error from provider | Navigate with `?error=access_denied` | Error displayed: "access_denied" or description | P0 |
| U-CB-003 | No code parameter | Navigate to `/callback` without params | Error: "No authorization code received" | P0 |
| U-CB-004 | Loading spinner | Observe during code exchange | Spinner shown while "Completing sign in..." | P2 |
| U-CB-005 | Invalid authorization code | Navigate with `?code=invalid_code` | Error message from token exchange failure | P0 |
| U-CB-006 | Expired authorization code | Use code after 10-minute expiry | Error message | P1 |
| U-CB-007 | Back to login link | Click "Back to Login" | Navigate to `/login` | P1 |

### 2.2 GitHub Callback (`/callback/github`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-GH-001 | Valid GitHub code | GitHub redirects with valid code | User authenticated via GitHub, redirected to Dashboard | P1 |
| U-GH-002 | GitHub auth denied | User denies GitHub authorization | Error message displayed | P1 |
| U-GH-003 | New GitHub user | First-time GitHub login | Account auto-created with GitHub profile info | P1 |

### 2.3 Consent Page (`/consent`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-CON-001 | Approve consent | Review requested scopes, click Approve | Authorization code returned to client | P0 |
| U-CON-002 | Deny consent | Click Deny | Error returned to client, user redirected | P0 |
| U-CON-003 | Scope display | View consent page | All requested scopes listed with descriptions | P0 |
| U-CON-004 | No scopes requested | Consent page with no scopes | Minimal consent or handled gracefully | P2 |

### 2.4 Device Verification (`/device/verify`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-DV-001 | Valid device code | Enter valid device code, verify | Device authorized | P0 |
| U-DV-002 | Invalid device code | Enter wrong code | Error: invalid or expired code | P0 |
| U-DV-003 | Expired device code | Enter expired code | Error message | P1 |
| U-DV-004 | Empty device code | Submit without entering code | Validation error | P1 |

---

## Module 3: Account Pages (Protected)

### 3.1 Dashboard (`/`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-DASH-001 | Dashboard loads | Login, view Dashboard | Welcome message with username, Account ID, Email, Roles displayed | P0 |
| U-DASH-002 | No roles | User with no assigned roles | "None" displayed in roles section | P1 |
| U-DASH-003 | Multiple roles | User with admin + user roles | Both role badges displayed | P1 |
| U-DASH-004 | Quick links | Click Edit Profile / Security / Authorized Apps | Navigates to correct page | P0 |
| U-DASH-005 | Unauthenticated access | Navigate to `/` without auth | Redirect to `/login?redirect=/` | P0 |
| U-DASH-006 | Session restore | Reopen browser to `/` with valid session | Session restored via `auth.restoreSession()`, Dashboard shown | P0 |
| U-DASH-007 | Session restore failure | Reopen browser with expired session | Redirect to login with redirect param | P1 |

### 3.2 Profile Page (`/profile`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-PROF-001 | Profile loads | Navigate to Profile | Username, Account ID, Email, Verification status, Roles shown | P0 |
| U-PROF-002 | Email verified | User with verified email | Green "Verified" badge | P1 |
| U-PROF-003 | Email unverified | User with unverified email | Yellow "Unverified" badge, "Resend verification" link shown | P0 |
| U-PROF-004 | Resend verification | Click "Resend verification email" | Success: "Verification email sent!", disappears after 3s | P0 |
| U-PROF-005 | Resend verification failure | Simulate API failure | Error message displayed | P1 |
| U-PROF-006 | No email | User without email | "N/A" displayed for email, no resend link | P2 |
| U-PROF-007 | API failure | Simulate GET /api/me failure | Error: "Failed to load profile" | P0 |
| U-PROF-008 | Loading state | Observe during page load | "Loading..." placeholder shown | P2 |

### 3.3 Security Page (`/security`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-SEC-001 | Page loads | Navigate to Security | Password change form, MFA section, WebAuthn section displayed | P0 |
| U-SEC-002 | Change password - valid | Enter old + new password (6+ chars, matching), submit | Success message, fields cleared | P0 |
| U-SEC-003 | Change password - mismatch | New password != confirm | Error: "Passwords do not match" | P0 |
| U-SEC-004 | Change password - too short | New password < 6 chars | Error: "Password must be at least 6 characters" | P0 |
| U-SEC-005 | Change password - wrong old | Enter incorrect old password | Error message from API | P0 |
| U-SEC-006 | Change password - empty fields | Submit with empty old password | Form validation or API error | P1 |
| U-SEC-007 | MFA setup | Click "Setup MFA" | QR code and secret key displayed | P0 |
| U-SEC-008 | MFA verify - valid code | Enter correct TOTP code after setup | Success: "MFA enabled successfully!", MFA now active | P0 |
| U-SEC-009 | MFA verify - invalid code | Enter wrong code | Error message | P0 |
| U-SEC-010 | MFA disable - valid password | Enter password, click Disable MFA | Success: "MFA disabled" | P0 |
| U-SEC-011 | MFA disable - empty password | Click Disable without entering password | Error: "Password required to disable MFA" | P1 |
| U-SEC-012 | MFA disable - wrong password | Enter wrong password | Error message from API | P0 |
| U-SEC-013 | WebAuthn register | Click "Register Passkey" | Browser WebAuthn dialog shown | P1 |
| U-SEC-014 | WebAuthn register cancel | Cancel browser dialog | Error: "Passkey registration was cancelled or timed out" | P1 |
| U-SEC-015 | WebAuthn not supported | Access from unsupported browser | WebAuthn section hidden or "not supported" message | P1 |
| U-SEC-016 | Delete account - correct username | Enter matching username, click Delete | Account deleted, redirected to login | P0 |
| U-SEC-017 | Delete account - wrong username | Enter non-matching username | Error: "Username does not match" | P0 |
| U-SEC-018 | Delete account - empty username | Click Delete without username | Validation prevents submission | P1 |
| U-SEC-019 | Loading states | Submit any form | Buttons show loading state, disabled during request | P2 |

### 3.4 Authorized Apps (`/authorized-apps`)

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-APP-001 | List authorized apps | Navigate to Authorized Apps | Each app shows name, client_id, scopes, Revoke button | P0 |
| U-APP-002 | Empty list | User has no authorized apps | "No authorized applications" empty state | P1 |
| U-APP-003 | Revoke app | Click Revoke, confirm dialog | App removed from list, success message | P0 |
| U-APP-004 | Revoke cancel | Click Revoke, cancel confirm | App remains in list | P1 |
| U-APP-005 | Revoke failure | Simulate API failure on revoke | Error message displayed | P0 |
| U-APP-006 | App without name | App has client_id but no name | client_id displayed as fallback name | P1 |
| U-APP-007 | Success message auto-dismiss | After successful revoke | Success message disappears after 3s | P2 |
| U-APP-008 | Loading state | During initial load | "Loading..." placeholder shown | P2 |

---

## Module 4: Navigation & Layout

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-NAV-001 | Top navigation links | Click Overview/Profile/Security/Authorized Apps | Correct page loads, active link highlighted | P0 |
| U-NAV-002 | Logo link | Click logo | Navigate to Dashboard | P1 |
| U-NAV-003 | User dropdown | Click user avatar | Dropdown with Profile, Security, Sign Out | P0 |
| U-NAV-004 | Dropdown navigation | Click Profile in dropdown | Navigate to `/profile`, dropdown closes | P1 |
| U-NAV-005 | Click outside dropdown | Open dropdown, click outside | Dropdown closes | P1 |
| U-NAV-006 | Logout from dropdown | Click "Sign Out" | Session cleared, redirect to login | P0 |
| U-NAV-007 | Sticky header | Scroll page content | Header remains visible at top | P2 |
| U-NAV-008 | Responsive nav | Resize to mobile width | Nav collapses to hamburger or minimal layout | P1 |
| U-NAV-009 | Active nav state | Navigate to `/security` | "Security" nav link highlighted with indigo background | P1 |

---

## Module 5: Cross-Cutting Concerns

### 5.1 Error Handling

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-ERR-001 | Network error | Disable network during API call | Error message displayed, no crash | P0 |
| U-ERR-002 | 401 Unauthorized | Let session expire, make API call | Redirect to login page | P0 |
| U-ERR-003 | 500 Server error | Trigger server error | Error message, no crash | P0 |
| U-ERR-004 | Success message auto-dismiss | Perform successful action | Success message disappears after 3-4 seconds | P2 |
| U-ERR-005 | Error normalization | Receive non-standard API error | Error normalized via `errorAdapter`, user-friendly message | P1 |

### 5.2 Security

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-SEC-001 | Route guard - protected pages | Navigate to `/profile` without auth | Redirect to `/login?redirect=/profile` | P0 |
| U-SEC-002 | Route guard - guest pages | Navigate to `/login` while authenticated | Redirect to Dashboard | P0 |
| U-SEC-003 | Token not in URL | After login | Access token not visible in URL | P0 |
| U-SEC-004 | Password field masking | View all password fields | Type="password", characters masked | P1 |
| U-SEC-005 | Anti-enumeration (forgot password) | Submit unregistered email | Same success message as registered email | P0 |
| U-SEC-006 | CSRF on password change | Change password request | Proper auth headers included | P0 |
| U-SEC-007 | localStorage cleared on logout | Logout, check localStorage | Auth tokens removed | P0 |
| U-SEC-008 | localStorage cleared on account delete | Delete account | localStorage.clear() called | P0 |

### 5.3 Session Management

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-SESS-001 | Session restore on page reload | Refresh page while logged in | Session restored, no redirect to login | P0 |
| U-SESS-002 | Session expire during use | Wait for token expiry, make API call | Redirect to login | P0 |
| U-SESS-003 | Multiple tabs | Open app in two tabs, logout in one | Other tab redirects to login on next navigation | P1 |
| U-SESS-004 | Token refresh | Access token expires, refresh token valid | New access token obtained seamlessly | P1 |

### 5.4 Performance

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-PERF-001 | Lazy-loaded routes | Check network tab during navigation | Only required chunks loaded (code splitting) | P2 |
| U-PERF-002 | Large authorized apps list | User with 50+ authorized apps | Page renders without lag | P1 |
| U-PERF-003 | WebAuthn credential list | User with many registered passkeys | Credentials listed efficiently | P2 |

### 5.5 Accessibility

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-A11Y-001 | Form labels | Inspect all form inputs | All inputs have associated labels | P1 |
| U-A11Y-002 | Required field indicators | View registration form | Required fields marked (asterisk or "required" attribute) | P1 |
| U-A11Y-003 | Keyboard navigation | Tab through login form | All interactive elements reachable, focus visible | P1 |
| U-A11Y-004 | Submit on Enter | Press Enter in password field | Form submits | P1 |
| U-A11Y-005 | Error announcement | Trigger form error | Error message readable by screen reader | P2 |
| U-A11Y-006 | Autocomplete attributes | Inspect login form | autocomplete="username", "current-password" set correctly | P2 |

---

## Module 6: Edge Cases & Stress Scenarios

| ID | Test Case | Steps | Expected Result | Priority |
|----|-----------|-------|-----------------|----------|
| U-EDGE-001 | Unicode username | Register with `用户名` as username | Handled correctly or validation error | P1 |
| U-EDGE-002 | Email with + addressing | Register with `user+tag@example.com` | Accepted and handled correctly | P1 |
| U-EDGE-003 | Very long password | Enter 1000-char password | Accepted or validation error with message | P2 |
| U-EDGE-004 | Special characters in password | Enter `P@$$w0rd!#%^&*()` | Password accepted | P1 |
| U-EDGE-005 | Browser back after logout | Logout, press browser Back | Redirect to login (no cached authenticated page) | P1 |
| U-EDGE-006 | Direct URL to OAuth callback | Navigate to `/callback` directly | Error: "No authorization code received" | P1 |
| U-EDGE-007 | Double-click submit | Rapidly double-click Sign In | Only one request sent (button disabled) | P2 |
| U-EDGE-008 | Slow network | Login on slow 3G connection | Loading state shown, eventually completes or times out | P2 |
| U-EDGE-009 | WebAuthn in HTTP context | Access via HTTP (not HTTPS) | WebAuthn gracefully unavailable | P1 |
| U-EDGE-010 | Multiple MFA setup attempts | Click Setup MFA multiple times | Only one setup flow active at a time | P2 |
