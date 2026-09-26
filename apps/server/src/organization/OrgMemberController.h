#pragma once

// v1.4.0 organization membership routes (see OrgMemberService.h for the
// permission model). Product-level, OAuth2AuthFilter-guarded (user tokens
// with `profile` scope, same family as the other /api/me routes).

#include <drogon/HttpController.h>

namespace organization
{

class OrgMemberController : public ::drogon::HttpController<OrgMemberController, false>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(
      OrgMemberController::createOrg,
      "/api/me/organizations",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::listMyOrgs,
      "/api/me/organizations",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::listMembers,
      "/api/me/organizations/{slug}/members",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::removeMember,
      "/api/me/organizations/{slug}/members/{userId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::createInvitation,
      "/api/me/organizations/{slug}/invitations",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::listInvitations,
      "/api/me/organizations/{slug}/invitations",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::revokeInvitation,
      "/api/me/organizations/{slug}/invitations/{invitationId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // Literal path (not under {slug}) so it can never be shadowed by the
    // slug-parameterized routes above.
    ADD_METHOD_TO(
      OrgMemberController::acceptInvitation,
      "/api/me/org-invitations/accept",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // v1.5.0 M2 (design §2.2, R-M2-4): org-consent management surface.
    ADD_METHOD_TO(
      OrgMemberController::listOrgConsents,
      "/api/me/organizations/{slug}/consents",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::revokeOrgConsents,
      "/api/me/organizations/{slug}/consents/{clientId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // v1.5.0 M3 (design §1.3 item 2, R-M3-3): ownership succession.
    ADD_METHOD_TO(
      OrgMemberController::nominateSuccessor,
      "/api/me/organizations/{slug}/successor-nomination",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::withdrawSuccessionNomination,
      "/api/me/organizations/{slug}/successor-nomination",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // Literal path so the accept route can never be captured by the
    // nomination resource routes above.
    ADD_METHOD_TO(
      OrgMemberController::acceptSuccession,
      "/api/me/organizations/{slug}/successor-nomination/accept",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    // #236 plan B: member-files-manager-approves org consent requests.
    // consent-requests is a new 4th-segment literal; drogon's whole-segment
    // matching keeps it disjoint from /consents/{clientId}.
    ADD_METHOD_TO(
      OrgMemberController::fileConsentRequest,
      "/api/me/organizations/{slug}/consent-requests",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::listConsentRequests,
      "/api/me/organizations/{slug}/consent-requests",
      ::drogon::Get,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::approveConsentRequest,
      "/api/me/organizations/{slug}/consent-requests/{requestId}/approve",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::rejectConsentRequest,
      "/api/me/organizations/{slug}/consent-requests/{requestId}/reject",
      ::drogon::Post,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    ADD_METHOD_TO(
      OrgMemberController::withdrawConsentRequest,
      "/api/me/organizations/{slug}/consent-requests/{requestId}",
      ::drogon::Delete,
      "fulla::drogon::filters::OAuth2AuthFilter"
    );
    METHOD_LIST_END

    void createOrg(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void listMyOrgs(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );
    void listMembers(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );
    void removeMember(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &userId
    );
    void createInvitation(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );
    void listInvitations(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );
    void revokeInvitation(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &invitationId
    );
    void acceptInvitation(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback
    );

    /// GET /api/me/organizations/{slug}/consents — active org consents
    /// grouped by client (owner/admin).
    void listOrgConsents(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// DELETE /api/me/organizations/{slug}/consents/{clientId} — revoke
    /// every active consent of the (org, client) pair (owner/admin).
    void revokeOrgConsents(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &clientId
    );

    /// POST /api/me/organizations/{slug}/successor-nomination — owner
    /// nominates a successor (body {user_id}, v1.5.0 M3).
    void nominateSuccessor(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// DELETE /api/me/organizations/{slug}/successor-nomination — owner
    /// withdraws the pending nomination.
    void withdrawSuccessionNomination(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// POST /api/me/organizations/{slug}/successor-nomination/accept —
    /// the nominee accepts; single-transaction seat swap.
    void acceptSuccession(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// POST /api/me/organizations/{slug}/consent-requests {client_id} —
    /// a member files an org-consent request (idempotent per B3, #236).
    void fileConsentRequest(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// GET /api/me/organizations/{slug}/consent-requests — pending
    /// requests with requester names (owner/admin).
    void listConsentRequests(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug
    );

    /// POST .../consent-requests/{requestId}/approve — owner/admin; writes
    /// the organization_consents rows and auto-approves siblings (B4).
    void approveConsentRequest(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &requestId
    );

    /// POST .../consent-requests/{requestId}/reject — owner/admin; optional
    /// body {reason}.
    void rejectConsentRequest(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &requestId
    );

    /// DELETE .../consent-requests/{requestId} — the requester withdraws
    /// their own pending request (B7 anti-enumeration 404 otherwise).
    void withdrawConsentRequest(
      const ::drogon::HttpRequestPtr &req,
      std::function<void(const ::drogon::HttpResponsePtr &)> &&callback,
      const std::string &slug,
      const std::string &requestId
    );

    /// #43 pattern: declare the /api/me org routes' scope requirements for
    /// the OpenAPI generator (same initApiDocs discipline as the SDK
    /// controllers).
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace organization
