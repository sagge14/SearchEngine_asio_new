#include "InteractivePaths.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <ShlObj.h>

#include <stdexcept>

namespace searchengine_archive {
namespace {

fs::path existingPickerStart(fs::path candidate)
{
    std::error_code error;
    if (fs::is_regular_file(candidate, error))
        candidate = candidate.parent_path();
    error.clear();
    while (!candidate.empty() && !fs::is_directory(candidate, error)) {
        error.clear();
        const fs::path parent = candidate.parent_path();
        if (parent == candidate)
            return {};
        candidate = parent;
    }
    return candidate;
}

class ComReleaser final
{
public:
    explicit ComReleaser(IUnknown* value = nullptr) : value_(value) {}
    ~ComReleaser()
    {
        if (value_)
            value_->Release();
    }
    ComReleaser(const ComReleaser&) = delete;
    ComReleaser& operator=(const ComReleaser&) = delete;

private:
    IUnknown* value_{};
};

class ComApartment final
{
public:
    ComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = SUCCEEDED(result);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("cannot initialize COM folder picker");
    }

    ~ComApartment()
    {
        if (uninitialize_)
            CoUninitialize();
    }

private:
    bool uninitialize_{false};
};

} // namespace

DirectoryInputResult resolveDirectoryInput(
    const std::wstring& input,
    const fs::path& suggestedDirectory,
    bool allowDisable,
    const FolderPickerCallback& picker)
{
    if (input == L"-") {
        if (!allowDisable)
            throw std::invalid_argument("'-' is allowed only for an optional directory");
        return {DirectoryInputAction::Disabled, {}};
    }
    if (!input.empty())
        return {DirectoryInputAction::Selected, fs::path(input)};
    if (!picker)
        throw std::invalid_argument("folder picker callback is missing");
    const auto selected = picker(suggestedDirectory);
    if (!selected || selected->empty())
        return {DirectoryInputAction::PickerCancelled, {}};
    return {DirectoryInputAction::Selected, *selected};
}

std::optional<fs::path> pickFolderWindows7(const fs::path& suggestedDirectory)
{
    ComApartment apartment;

    IFileOpenDialog* rawDialog = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&rawDialog));
    if (FAILED(created)) {
        throw std::runtime_error("cannot create Windows folder picker");
    }
    ComReleaser dialogGuard(rawDialog);

    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(rawDialog->GetOptions(&options)) ||
        FAILED(rawDialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)))
    {
        throw std::runtime_error("cannot configure Windows folder picker");
    }

    const fs::path initial = existingPickerStart(suggestedDirectory);
    if (!initial.empty()) {
        IShellItem* rawInitial = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial.c_str(), nullptr, IID_PPV_ARGS(&rawInitial))))
        {
            ComReleaser initialGuard(rawInitial);
            rawDialog->SetFolder(rawInitial);
        }
    }

    const HRESULT shown = rawDialog->Show(GetConsoleWindow());
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return std::nullopt;
    }
    if (FAILED(shown)) {
        throw std::runtime_error("Windows folder picker failed");
    }

    IShellItem* rawResult = nullptr;
    if (FAILED(rawDialog->GetResult(&rawResult)) || !rawResult) {
        throw std::runtime_error("folder picker returned no result");
    }
    ComReleaser resultGuard(rawResult);
    PWSTR rawPath = nullptr;
    if (FAILED(rawResult->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
        throw std::runtime_error("selected folder has no filesystem path");
    }
    const fs::path result(rawPath);
    CoTaskMemFree(rawPath);
    return result;
}

bool shouldDeleteRestoredArchive(
    bool assumeYes,
    bool interactiveConfirmation) noexcept
{
    return !assumeYes && interactiveConfirmation;
}

} // namespace searchengine_archive
