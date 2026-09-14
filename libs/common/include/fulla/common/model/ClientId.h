#pragma once

// Task 13 (fulla-sdk-refactor, design.md §3.1/§5.1/§6): value objects
// for the shared Domain kernel. ClientId wraps the OAuth2 client_id string
// (design.md §3.1's value-object list). No format grammar is imposed
// beyond non-empty: RFC 6749 deliberately leaves client_id opaque to the
// protocol, and the existing codebase's client_ids (e.g. "fulla-portal",
// "backend-svc") are plain human-chosen slugs, not a generated format this
// type should validate against.

#include <stdexcept>
#include <string>
#include <utility>

namespace fulla::common::model
{

/**
 * @brief Opaque, immutable OAuth2 client identifier.
 */
class ClientId
{
  public:
    /// Construct from a raw client_id string. Throws std::invalid_argument
    /// if `value` is empty.
    explicit ClientId(std::string value) : value_(std::move(value))
    {
        if (value_.empty())
        {
            throw std::invalid_argument("ClientId: value must not be empty");
        }
    }

    const std::string &value() const noexcept
    {
        return value_;
    }

    bool operator==(const ClientId &other) const noexcept
    {
        return value_ == other.value_;
    }

    bool operator!=(const ClientId &other) const noexcept
    {
        return !(*this == other);
    }

  private:
    std::string value_;
};

}  // namespace fulla::common::model
