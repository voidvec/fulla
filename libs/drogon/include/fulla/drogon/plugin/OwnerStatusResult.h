#pragma once

#include <optional>
#include <string>
#include <utility>

namespace fulla::drogon
{
enum class OwnerStatusCode
{
    Active,
    Unmanaged,
    Suspended,
    Unavailable
};

struct OwnerStatusResult
{
    OwnerStatusCode code = OwnerStatusCode::Unavailable;
    // Preserve the exact row value for the legacy display-only wrapper.
    std::optional<std::string> value;

    static OwnerStatusResult fromStatus(std::optional<std::string> status)
    {
        const auto state = !status               ? OwnerStatusCode::Unmanaged
                           : *status == "active" ? OwnerStatusCode::Active
                                                 : OwnerStatusCode::Suspended;
        return {state, std::move(status)};
    }

    bool permitsIntrospection() const
    {
        return code == OwnerStatusCode::Active || code == OwnerStatusCode::Unmanaged;
    }
};
}  // namespace fulla::drogon
