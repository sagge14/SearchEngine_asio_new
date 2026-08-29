#pragma once

#include <filesystem>

#include "FileWatcher/Commands/IFileEventCommand.h"
#include "ZagKpodiChanger.h"
#include "ZagEditorLog.h"

namespace zag_editor {

// Адаптер для FileEventDispatcher: на событие Added-like (dispatcher сам мапит Modified/RenamedNew в Added)
// выполняет замену From= в .zag файле.
class ZagProcessCommand final : public IFileEventCommand {
public:
    ZagProcessCommand(ZagKpodiChanger& changer, bool backup, MinuteThrottledLogger& logger);

    void execute(const std::wstring& path) override;

private:
    ZagKpodiChanger& changer_;
    bool backup_{false};
    MinuteThrottledLogger& logger_;

    static bool copyBakIfNeeded(const std::filesystem::path& src, bool enabled);
    static bool hasZagExtension(const std::filesystem::path& p);
};

} // namespace zag_editor

