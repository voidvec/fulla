#include <drogon/drogon_test.h>
#include <fulla/drogon/plugin/OAuth2Plugin.h>

using fulla::drogon::OwnerStatusCode;
using fulla::drogon::OwnerStatusResult;

DROGON_TEST(Unit_P0_OwnerStatus_OnlyActiveOrConfirmedUnmanagedPermitsIntrospection)
{
    auto active = OwnerStatusResult::fromStatus("active");
    CHECK(active.code == OwnerStatusCode::Active);
    CHECK(active.permitsIntrospection());
    for (const char *status : {"suspended", "deleted", "", "unknown-future-status"})
    {
        auto result = OwnerStatusResult::fromStatus(status);
        CHECK(result.code == OwnerStatusCode::Suspended);
        CHECK(!result.permitsIntrospection());
    }
    auto unmanaged = OwnerStatusResult::fromStatus(std::nullopt);
    CHECK(unmanaged.code == OwnerStatusCode::Unmanaged);
    CHECK(unmanaged.permitsIntrospection());
    OwnerStatusResult unavailable;
    CHECK(unavailable.code == OwnerStatusCode::Unavailable);
    CHECK(!unavailable.permitsIntrospection());
}

DROGON_TEST(Unit_P0_OwnerStatus_MissingBackendIsUnavailable)
{
    auto plugin = std::make_shared<OAuth2Plugin>();
    int calls = 0;
    OwnerStatusResult result;
    plugin->lookupOwnerStatusChecked("no-backend", [&calls, &result](OwnerStatusResult value) {
        ++calls;
        result = std::move(value);
    });
    CHECK(calls == 1);
    CHECK(result.code == OwnerStatusCode::Unavailable);
    CHECK(!result.permitsIntrospection());
}
