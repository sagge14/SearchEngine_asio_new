#include "QueryWordMatch.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace search_server {

std::optional<QueryWordMatch> parseQueryWordMatch(std::string_view value)
{
    if (value == "all") {
        return QueryWordMatch::All;
    }
    if (value == "any") {
        return QueryWordMatch::Any;
    }
    return std::nullopt;
}

std::string_view toString(QueryWordMatch value) noexcept
{
    switch (value) {
    case QueryWordMatch::All:
        return "all";
    case QueryWordMatch::Any:
        return "any";
    }
    return "any";
}

QueryPostingSet combineQueryPostings(
    QueryWordMatch mode,
    const std::vector<std::optional<QueryPostingSet>>& postings)
{
    if (postings.empty()) {
        return {};
    }

    if (mode == QueryWordMatch::Any) {
        QueryPostingSet result;
        for (const auto& posting : postings) {
            if (posting) {
                result.insert(posting->begin(), posting->end());
            }
        }
        return result;
    }

    std::vector<const QueryPostingSet*> present;
    present.reserve(postings.size());
    for (const auto& posting : postings) {
        if (!posting) {
            return {};
        }
        present.push_back(&*posting);
    }
    std::sort(
        present.begin(), present.end(),
        [](const QueryPostingSet* left, const QueryPostingSet* right) {
            return left->size() < right->size();
        });

    QueryPostingSet result = *present.front();
    for (std::size_t index = 1; index < present.size(); ++index) {
        QueryPostingSet intersection;
        std::set_intersection(
            result.begin(), result.end(),
            present[index]->begin(), present[index]->end(),
            std::inserter(intersection, intersection.end()));
        result = std::move(intersection);
        if (result.empty()) {
            break;
        }
    }
    return result;
}

}  // namespace search_server
