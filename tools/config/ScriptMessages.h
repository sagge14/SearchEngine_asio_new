#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace script_messages {

std::wstring render(
    std::wstring_view id,
    bool russian,
    const std::vector<std::wstring>& arguments);

void validateCatalog();

} // namespace script_messages
