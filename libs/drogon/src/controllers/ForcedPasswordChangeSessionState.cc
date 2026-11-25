#include "ForcedPasswordChangeSessionState.h"

#include <drogon/Session.h>

namespace fulla::drogon::controllers::detail
{
void clearForcedPasswordChangeSessionAndRotate(::drogon::Session &session)
{
    session.erase("must_change_password");
    session.erase("userId");
    session.erase("sub");
    session.erase("auth_time");
    session.erase("amr");
    session.erase("mfa_pending");
    session.erase("mfa_code_challenge");
    session.erase("mfa_code_challenge_method");
    // Rotate once. The framework retains the old ID for in-flight requests.
    session.changeSessionIdToClient();
}
}  // namespace fulla::drogon::controllers::detail
