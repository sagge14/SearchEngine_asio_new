#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace searchengine_archive {

namespace fs = std::filesystem;

enum class DirectoryInputAction
{
    Selected,
    Disabled,
    PickerCancelled
};

struct DirectoryInputResult
{
    DirectoryInputAction action{DirectoryInputAction::PickerCancelled};
    fs::path path;
};

using FolderPickerCallback =
    std::function<std::optional<fs::path>(const fs::path& initialDirectory)>;

[[nodiscard]] DirectoryInputResult resolveDirectoryInput(
    const std::wstring& input,
    const fs::path& suggestedDirectory,
    bool allowDisable,
    const FolderPickerCallback& picker);

[[nodiscard]] std::optional<fs::path> pickFolderWindows7(
    const fs::path& suggestedDirectory);

/// --yes confirms the non-destructive restore only. Archive deletion always
/// requires a separate interactive answer after manual verification.
[[nodiscard]] bool shouldDeleteRestoredArchive(
    bool assumeYes,
    bool interactiveConfirmation) noexcept;

} // namespace searchengine_archive
