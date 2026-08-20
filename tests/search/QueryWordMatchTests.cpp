#include "SearchServer/QueryWordMatch.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace {

using search_server::QueryPostingSet;
using search_server::QueryWordMatch;

TEST(QueryWordMatchTest, ParsesOnlyCanonicalValues)
{
    EXPECT_EQ(search_server::parseQueryWordMatch("all"), QueryWordMatch::All);
    EXPECT_EQ(search_server::parseQueryWordMatch("any"), QueryWordMatch::Any);
    EXPECT_FALSE(search_server::parseQueryWordMatch("and"));
    EXPECT_FALSE(search_server::parseQueryWordMatch("ALL"));
    EXPECT_EQ(search_server::toString(QueryWordMatch::All), "all");
    EXPECT_EQ(search_server::toString(QueryWordMatch::Any), "any");
}

TEST(QueryWordMatchTest, AllIntersectsPostings)
{
    const auto result = search_server::combineQueryPostings(
        QueryWordMatch::All,
        {QueryPostingSet{1, 2, 3}, QueryPostingSet{2, 3, 4}});
    EXPECT_EQ(result, (QueryPostingSet{2, 3}));
}

TEST(QueryWordMatchTest, AnyUnionsPostings)
{
    const auto result = search_server::combineQueryPostings(
        QueryWordMatch::Any,
        {QueryPostingSet{1, 2}, QueryPostingSet{2, 3}});
    EXPECT_EQ(result, (QueryPostingSet{1, 2, 3}));
}

TEST(QueryWordMatchTest, MissingWordRejectsAllButIsIgnoredByAny)
{
    const std::vector<std::optional<QueryPostingSet>> postings{
        QueryPostingSet{1, 2}, std::nullopt};
    EXPECT_TRUE(search_server::combineQueryPostings(
        QueryWordMatch::All, postings).empty());
    EXPECT_EQ(search_server::combineQueryPostings(
        QueryWordMatch::Any, postings), (QueryPostingSet{1, 2}));
}

TEST(QueryWordMatchTest, AllMissingAndEmptyRequestsReturnNothing)
{
    const std::vector<std::optional<QueryPostingSet>> missing{
        std::nullopt, std::nullopt};
    EXPECT_TRUE(search_server::combineQueryPostings(
        QueryWordMatch::All, missing).empty());
    EXPECT_TRUE(search_server::combineQueryPostings(
        QueryWordMatch::Any, missing).empty());
    EXPECT_TRUE(search_server::combineQueryPostings(
        QueryWordMatch::All, {}).empty());
    EXPECT_TRUE(search_server::combineQueryPostings(
        QueryWordMatch::Any, {}).empty());
}

TEST(QueryWordMatchTest, OneExistingWordIsEquivalent)
{
    const std::vector<std::optional<QueryPostingSet>> one{
        QueryPostingSet{7, 9}};
    EXPECT_EQ(search_server::combineQueryPostings(
        QueryWordMatch::All, one), (QueryPostingSet{7, 9}));
    EXPECT_EQ(search_server::combineQueryPostings(
        QueryWordMatch::Any, one), (QueryPostingSet{7, 9}));
}

}  // namespace
