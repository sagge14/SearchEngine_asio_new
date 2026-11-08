#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace zag_editor {

struct Settings {
    std::filesystem::path dict_path{};
    bool recursive{true};
    bool backup{false};
    std::vector<std::filesystem::path> input_dirs{};
};

struct LoadedSettings {
    Settings settings{};
    bool loaded_from_file{false};
};

// Простое INI: key=value, без секций. Комментарии: строки с # или ; (в начале).
LoadedSettings loadSettingsIni(const std::filesystem::path& ini_path);

} // namespace zag_editor

