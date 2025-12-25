#include "NumberSets.h"
#include <charconv>
#include <stdexcept>
#include <optional>

/* ---------- helpers ---------- */
namespace {
    // конвертируем строковое значение первого столбца в int (без locale/try-catch)
    std::optional<int> strToInt(std::string_view sv)
    {
        int value{};
        auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
        return (ec == std::errc{}) ? std::optional<int>{value} : std::nullopt;
    }
}

/* ---------- private ---------- */

void NumberSets::fillSet(mySQLite&               db,
                         std::unordered_set<int>& target,
                         const std::string&       table) const
{
    const std::string sql = "SELECT " + num_field_ + " FROM " + table;
    db.execSql(sql);

    target.clear();
    for (const auto& row : db)          // row — std::map<string,string>
        if (auto n = strToInt(row.begin()->second); n)   // первый столбец
            target.insert(*n);
}

/* ---------- public ---------- */

NumberSets::NumberSets(std::string db_path,
                       std::string vh_table,
                       std::string ish_table,
                       std::string num_field)
        : db_path_(std::move(db_path))
        , vh_table_(std::move(vh_table))
        , ish_table_(std::move(ish_table))
        , num_field_(std::move(num_field))
{
    reload();
}

void NumberSets::reload()
{
    mySQLite db(db_path_);          // одно соединение – переиспользуем

    fillSet(db, vh_set_,  vh_table_);
    fillSet(db, ish_set_, ish_table_);
}

bool NumberSets::containsVH(int num)  const noexcept { return vh_set_.contains(num); }
bool NumberSets::containsISH(int num) const noexcept { return ish_set_.contains(num); }
bool NumberSets::containsAny(int num) const noexcept { return containsVH(num) || containsISH(num); }
