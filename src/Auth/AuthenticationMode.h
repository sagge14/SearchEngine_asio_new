#pragma once

#include <string_view>

namespace auth
{
    enum class AuthenticationMode { Required, None };

    inline constexpr std::string_view kAnonymousUser = "unauthenticated_user";

    [[nodiscard]] inline constexpr std::string_view toString(
        AuthenticationMode mode) noexcept
    {
        return mode == AuthenticationMode::None ? "none" : "required";
    }
}
