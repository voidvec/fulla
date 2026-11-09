#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fulla::storage::postgres
{
enum class OrgConsentDecisionStatus
{
    Approved,
    AlreadyApproved,
    Rejected,
    NotFound,
    Forbidden,
    Conflict,
    StorageError
};

struct OrgConsentDecisionResult
{
    OrgConsentDecisionStatus status = OrgConsentDecisionStatus::StorageError;
    int32_t requestId = 0;
    std::string clientId;
    std::vector<std::string> scopes;
};
}  // namespace fulla::storage::postgres
