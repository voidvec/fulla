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

    /// #43 pattern: declare the /api/me org routes' scope requirements for
    /// the OpenAPI generator (same initApiDocs discipline as the SDK
    /// controllers).
    static void initApiDocs();

  private:
    static void initApiDocsImpl();
};

}  // namespace organization
