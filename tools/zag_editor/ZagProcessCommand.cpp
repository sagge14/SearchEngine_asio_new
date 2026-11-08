#include "ZagProcessCommand.h"

#include <fstream>
#include <algorithm>
#include <system_error>

namespace zag_editor {

ZagProcessCommand::ZagProcessCommand(ZagKpodiChanger& changer, bool backup, MinuteThrottledLogger& logger)
    : changer_(changer)
    , backup_(backup)
    , logger_(logger) {}

bool ZagProcessCommand::hasZagExtension(const std::filesystem::path& p) {
    auto ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".zag";
}

bool ZagProcessCommand::copyBakIfNeeded(const std::filesystem::path& src, bool enabled) {
    if (!enabled) return true;

    std::error_code ec;
    std::filesystem::path bak = src;
    bak += L".bak";
    std::filesystem::copy_file(src, bak, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

void ZagProcessCommand::execute(const std::wstring& path) {
    std::filesystem::path p(path);

    if (!hasZagExtension(p)) {
        // Dispatcher уже фильтрует по расширению, но оставим защиту.
        return;
    }

    if (!copyBakIfNeeded(p, backup_)) {
        logger_.logError(L"[ZagEditor] ERROR backup failed path=" + p.wstring());
        return;
    }

    bool ok = false;
    try {
        ok = changer_.processFile(p);
    } catch (const std::exception& e) {
        std::string what = e.what();
        std::wstring w(what.begin(), what.end());
        logger_.logError(L"[ZagEditor] ERROR exception: " + w);
        return;
    }

    if (ok) {
        logger_.logInfo(L"[ZagEditor] OK changed path=" + p.wstring());
    } else {
        logger_.logInfo(L"[ZagEditor] SKIP unchanged path=" + p.wstring());
    }
}

} // namespace zag_editor

