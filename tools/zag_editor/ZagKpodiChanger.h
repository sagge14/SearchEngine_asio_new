#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <ostream>

namespace zag_editor {

// Минимальная версия логики из ZagKpodiChengerCommand:
// - грузит словарь из EXPORT.INI
// - в .zag заменяет первую строку From= по словарю
class ZagKpodiChanger {
public:
    explicit ZagKpodiChanger(std::filesystem::path dict_path);

    // Возвращает true, если файл был изменён и перезаписан.
    bool processFile(const std::filesystem::path& zag_path) const;
    bool processFile(const std::filesystem::path& zag_path, std::ostream* verbose_log) const;

    // Для диагностики/проверок: сколько пар key->value загружено.
    [[nodiscard]] size_t dictSize() const { return mapKpodi_.size(); }

private:
    std::filesystem::path dict_path_;
    // Используем bytes (std::string), чтобы не зависеть от поведения wide-stream на разных toolchain.
    // Требование простое: кодировка в EXPORT.INI и .zag должна совпадать (как и в оригинальном решении).
    std::map<std::string, std::string> mapKpodi_;

    void loadMapOrThrow(const std::filesystem::path& dict_path);

    static bool starts_with(const std::string& s, const char* pref);
    static std::string trim(const std::string& s);
    static void stripTrailingCr(std::string& s);
    static std::string parse_s_key(const std::string& line);
};

} // namespace zag_editor

