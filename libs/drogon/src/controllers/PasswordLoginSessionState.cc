#include "PasswordLoginSessionState.h"

#include <drogon/Session.h>

namespace fulla::drogon::controllers::detail
{
void setPasswordLoginSubject(
  ::drogon::Session &session,
  int32_t internalId,
  const std::string &publicSub
)
{
    // Session::insert does not overwrite. Keep conversion after the erase,
    // and keep the internal ID distinct from the public OIDC subject.
    session.erase("userId");
    session.insert("userId", std::to_string(internalId));
    session.erase("sub");
    session.insert("sub", publicSub);
}

void setPasswordLoginClaimsAndRotate(::drogon::Session &session, int64_t authTime)
{
    // A new password authentication replaces any previous MFA claims.
    session.erase("auth_time");
    session.insert("auth_time", authTime);
    session.erase("amr");
    session.insert("amr", std::string("pwd"));
    session.erase("mfa_pending");
    session.erase("must_change_password");
    // Rotate once at the original privilege boundary. Other keys survive.
    session.changeSessionIdToClient();
}
}  // namespace fulla::drogon::controllers::detail
