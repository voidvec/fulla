#pragma once

namespace drogon
{
class Session;
}

namespace fulla::drogon::controllers::detail
{
// Clear only the forced-change authentication state at the existing boundary.
void clearForcedPasswordChangeSessionAndRotate(::drogon::Session &session);
}  // namespace fulla::drogon::controllers::detail
