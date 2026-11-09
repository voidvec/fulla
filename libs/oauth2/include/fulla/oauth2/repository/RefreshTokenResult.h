#pragma once

#include <fulla/oauth2/model/Dto.h>

#include <optional>

namespace fulla::oauth2::repository
{

enum class RefreshTokenStatus
{
    Ready,
    Rotated,
    NotFound,
    AlreadyUsed,
    ClientMismatch,
    Expired,
    StorageError
};

struct RefreshTokenResult
{
    RefreshTokenStatus status = RefreshTokenStatus::StorageError;
    std::optional<model::OAuth2RefreshToken> token;
};

}  // namespace fulla::oauth2::repository
