// AuditActions.h
//
// P2-2 batch A: the single source of truth for every audit EVENT name the
// product emits (the audit_logs.action column). Before this header the
// ~55 names lived as scattered string literals across 20+ files — the
// P2-2 webhook subscriptions need them as a CLOSED, spellable set (event
// filters validate against exactly these constants), and drift between
// emitters and filters would be silent.
//
// Contract:
//   * Names are lower_snake_case verbs/verb-phrases (subject_action).
//   * ONE historical outlier keeps its on-the-wire casing verbatim:
//     SOCIAL_LOGIN_TOKEN_ISSUED (landed uppercase in v1.6.0; changing the
//     stored value would break existing audit queries — normalize only
//     with a migration, tracked for v2.0).
//   * The constexpr kAllEvents array is the complete closed set — adding
//     a name without registering it in kAllEvents fails the unit test
//     (AuditActionsTest), which also fails if any constant's value drifts
//     out of the set's snake_case grammar.
//
// This header is in the common layer on purpose: both the Drogon-side
// emitters and the upcoming webhook delivery service (no Drogon dep)
// consume it.

#pragma once

#include <array>
#include <string_view>

namespace fulla::common::observability::audit_events
{

// ---- authentication / session -------------------------------------------
inline constexpr char kLoginSuccess[] = "login_success";
inline constexpr char kLoginFailure[] = "login_failure";
inline constexpr char kBackchannelLogout[] = "backchannel_logout";

// ---- token lifecycle ----------------------------------------------------
inline constexpr char kTokenIssued[] = "token_issued";
inline constexpr char kTokenRefreshed[] = "token_refreshed";
inline constexpr char kTokenRevoked[] = "token_revoked";
inline constexpr char kRefreshTokenReuseDetected[] = "refresh_token_reuse_detected";
inline constexpr char kRefreshSubjectNormalizationFailed[] = "refresh_subject_normalization_failed";
// Historical casing kept verbatim — see the header contract note.
inline constexpr char kSocialLoginTokenIssued[] = "SOCIAL_LOGIN_TOKEN_ISSUED";

// ---- MFA / WebAuthn -----------------------------------------------------
inline constexpr char kMfaEnabled[] = "mfa_enabled";
inline constexpr char kMfaVerified[] = "mfa_verified";
inline constexpr char kMfaBackupCodeUsed[] = "mfa_backup_code_used";
inline constexpr char kWebauthnRegistered[] = "webauthn_registered";
inline constexpr char kWebauthnAuthenticated[] = "webauthn_authenticated";
inline constexpr char kWebauthnCredentialDeleted[] = "webauthn_credential_deleted";
inline constexpr char kWebauthnCloneDetected[] = "webauthn_clone_detected";

// ---- account self-service / credentials ---------------------------------
inline constexpr char kPasswordChanged[] = "password_changed";
inline constexpr char kPasswordChangeFailed[] = "password_change_failed";
inline constexpr char kPasswordReset[] = "password_reset";
inline constexpr char kProfileUpdated[] = "profile_updated";
inline constexpr char kAccountDeleted[] = "account_deleted";
inline constexpr char kSocialAccountLinked[] = "social_account_linked";
inline constexpr char kSocialAccountUnlinked[] = "social_account_unlinked";
inline constexpr char kAppAuthorizationRevoked[] = "app_authorization_revoked";

// ---- admin surface -------------------------------------------------------
inline constexpr char kUserCreate[] = "user_create";
inline constexpr char kUserUpdate[] = "user_update";
inline constexpr char kUserDelete[] = "user_delete";
inline constexpr char kRolePermissionsUpdated[] = "role_permissions_updated";
inline constexpr char kScopeUpdated[] = "scope_updated";
inline constexpr char kClaimMappingCreated[] = "claim_mapping_created";
inline constexpr char kClaimMappingUpdated[] = "claim_mapping_updated";
inline constexpr char kClaimMappingDeleted[] = "claim_mapping_deleted";
inline constexpr char kEmailTemplateUpdated[] = "email_template_updated";
inline constexpr char kEmailTestSent[] = "email_test_sent";
inline constexpr char kAuditExported[] = "audit_exported";
inline constexpr char kClientRegistered[] = "client_registered";
inline constexpr char kClientSuspended[] = "client_suspended";

// ---- open platform (self-registered applications) ------------------------
inline constexpr char kApplicationCreated[] = "application_created";
inline constexpr char kApplicationSecretRotated[] = "application_secret_rotated";
inline constexpr char kApplicationTransferred[] = "application_transferred";
inline constexpr char kApplicationDeleted[] = "application_deleted";

// ---- organizations --------------------------------------------------------
inline constexpr char kOrganizationCreated[] = "organization_created";
inline constexpr char kOrgUpdated[] = "org_updated";
inline constexpr char kOrgDeleted[] = "org_deleted";
inline constexpr char kOrgOwnershipTransferred[] = "org_ownership_transferred";
inline constexpr char kOrgMemberRemoved[] = "org_member_removed";
inline constexpr char kOrgMemberLeft[] = "org_member_left";
inline constexpr char kOrgInvitationCreated[] = "org_invitation_created";
inline constexpr char kOrgInvitationRevoked[] = "org_invitation_revoked";
inline constexpr char kOrgConsentRevoked[] = "org_consent_revoked";
inline constexpr char kOrgConsentRequestFiled[] = "org_consent_request_filed";
inline constexpr char kOrgConsentRequestRejected[] = "org_consent_request_rejected";
inline constexpr char kOrgConsentRequestWithdrawn[] = "org_consent_request_withdrawn";
inline constexpr char kOrgSuccessorNominated[] = "org_successor_nominated";
inline constexpr char kOrgSuccessorAccepted[] = "org_successor_accepted";
inline constexpr char kOrgSuccessorNominationWithdrawn[] = "org_successor_nomination_withdrawn";
inline constexpr char kOrgSuccessorAutoEffected[] = "org_successor_auto_effected";

// The CLOSED set (webhook event filters validate against exactly this).
// Byte-sorted by VALUE (uppercase SOCIAL_LOGIN_TOKEN_ISSUED sorts first);
// the unit test enforces membership and ordering so binary-search
// validation stays available.
inline constexpr std::array<std::string_view, 57> kAllEvents{
  kSocialLoginTokenIssued,
  kAccountDeleted,
  kAppAuthorizationRevoked,
  kApplicationCreated,
  kApplicationDeleted,
  kApplicationSecretRotated,
  kApplicationTransferred,
  kAuditExported,
  kBackchannelLogout,
  kClaimMappingCreated,
  kClaimMappingDeleted,
  kClaimMappingUpdated,
  kClientRegistered,
  kClientSuspended,
  kEmailTemplateUpdated,
  kEmailTestSent,
  kLoginFailure,
  kLoginSuccess,
  kMfaBackupCodeUsed,
  kMfaEnabled,
  kMfaVerified,
  kOrgConsentRequestFiled,
  kOrgConsentRequestRejected,
  kOrgConsentRequestWithdrawn,
  kOrgConsentRevoked,
  kOrgDeleted,
  kOrgInvitationCreated,
  kOrgInvitationRevoked,
  kOrgMemberLeft,
  kOrgMemberRemoved,
  kOrgOwnershipTransferred,
  kOrgSuccessorAccepted,
  kOrgSuccessorAutoEffected,
  kOrgSuccessorNominated,
  kOrgSuccessorNominationWithdrawn,
  kOrgUpdated,
  kOrganizationCreated,
  kPasswordChangeFailed,
  kPasswordChanged,
  kPasswordReset,
  kProfileUpdated,
  kRefreshSubjectNormalizationFailed,
  kRefreshTokenReuseDetected,
  kRolePermissionsUpdated,
  kScopeUpdated,
  kSocialAccountLinked,
  kSocialAccountUnlinked,
  kTokenIssued,
  kTokenRefreshed,
  kTokenRevoked,
  kUserCreate,
  kUserDelete,
  kUserUpdate,
  kWebauthnAuthenticated,
  kWebauthnCloneDetected,
  kWebauthnCredentialDeleted,
  kWebauthnRegistered,
};

static_assert(kAllEvents.size() == 57, "register new audit events in kAllEvents");

// True iff `name` is a member of the closed set (the webhook subscription
// validator calls this; emitters should use the constants directly).
inline constexpr bool isKnownEvent(std::string_view name)
{
    for (auto event : kAllEvents)
    {
        if (event == name)
            return true;
    }
    return false;
}

}  // namespace fulla::common::observability::audit_events
