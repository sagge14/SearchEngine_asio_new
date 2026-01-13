#pragma once
#include "SQLite/mySQLite.h"
#include <unordered_set>
#include <string>

class NumberSets final
{
public:
    /**
     * @param db_path   — путь к общей БД
     * @param vh_table  — имя таблицы с номерами VH
     * @param ish_table — имя таблицы с номерами ISH
     * @param num_field — имя числового поля (по умолчанию "num")
     */
    NumberSets(std::string db_path,
               std::string vh_table,
               std::string ish_table,
               std::string num_field = "num");
    NumberSets() = default;
    /* моментальное членство */
    [[nodiscard]] bool containsVH(int num)  const noexcept;
    [[nodiscard]] bool containsISH(int num) const noexcept;
    [[nodiscard]] bool containsAny(int num) const noexcept;

    /* принудительная актуализация из БД */
    void reload();

private:
    std::string             db_path_;
    std::string             vh_table_;
    std::string             ish_table_;
    std::string             num_field_;
    std::unordered_set<int> vh_set_;
    std::unordered_set<int> ish_set_;

    void fillSet(mySQLite& db,
                 std::unordered_set<int>& target,
                 const std::string& table) const;
};
