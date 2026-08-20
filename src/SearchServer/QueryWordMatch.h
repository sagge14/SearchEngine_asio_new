#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace search_server {

enum class QueryWordMatch {
    All,
    Any,
};

std::optional<QueryWordMatch> parseQueryWordMatch(std::string_view value);
std::string_view toString(QueryWordMatch value) noexcept;

using QueryPostingSet = std::set<std::size_t>;

/// Missing words are represented by std::nullopt. All rejects any missing
/// word and intersects present postings; Any ignores missing words and unions
/// present postings.
QueryPostingSet combineQueryPostings(
    QueryWordMatch mode,
    const std::vector<std::optional<QueryPostingSet>>& postings);

}  // namespace search_server
