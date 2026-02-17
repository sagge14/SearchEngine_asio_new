//
// Created by Sg on 06.07.2025.
//

#ifndef SEARCHENGINE_INDEXCOMMANDS_H
#define SEARCHENGINE_INDEXCOMMANDS_H
#include "IFileEventCommand.h"
#include "UpdateOpisBaseCommand.h"
#include "SearchServer/SearchServer.h"
#include "ZagKpodiChenger.h"
#include "MyUtils/LogFile.h"
#include <filesystem>

template<typename TaskID>
class PeriodicTaskManager;

static bool isInDirWithWrongExt(const std::filesystem::path& p) {

    auto parent = p.parent_path().filename().wstring();
    if (parent.size() > 2 && parent.rfind(L"in", 0) == 0) { // начинается с "in"
        bool all_digits = std::all_of(parent.begin() + 2, parent.end(),
                                      [](wchar_t ch){ return iswdigit(ch); });
        if (all_digits) {
            // проверка расширения
            return !(p.has_extension() && p.extension() == L".zag");
        }
    }
    return false;
}

template<typename TaskID>
class AddFileCommand : public IFileEventCommand {
public:

    AddFileCommand(search_server::SearchServer& server, PeriodicTaskManager<TaskID>& ptm,
                   const std::vector<std::string>& ext)  : server_(server)
    {
            opis_command_ = std::make_unique<UpdateOpisBaseCommand<TaskID>>(ptm,ext);
            zag_kpodi_ = std::make_unique<ZagKpodiChengerCommand>();
    }

    void execute(const std::wstring& path) override {
        LogFile::getWatcher().write(L"[AddFileCommand] execute path=" + path);

        auto isZagFile = [](const std::wstring& path) {
            std::filesystem::path p(path);
            return p.has_extension() && p.extension() == L".zag";
        };



        if(isInDirWithWrongExt(path))
            return;


        if(isZagFile(path))
        {
            LogFile::getWatcher().write(L"[ZAG] = " + path);
            zag_kpodi_->execute(path);
            return;
        }



        server_.addFileToIndex(path);

        if (opis_command_) {
            try {
                opis_command_->execute(path);
            } catch (const std::exception& e) {
                LogFile::getWatcher().write(std::string("[execute] opis_command_ exception: ") + e.what());
            } catch (...) {
                LogFile::getWatcher().write("[execute] opis_command_ unknown exception!");
            }
        } else {
            LogFile::getWatcher().write(L"[execute] opis_command_ nullptr " + path);
        }



    }


private:

    search_server::SearchServer& server_;
    std::unique_ptr<UpdateOpisBaseCommand<TaskID>> opis_command_;
    std::unique_ptr<ZagKpodiChengerCommand> zag_kpodi_;
};

class RemoveFileCommand : public IFileEventCommand {
public:
    explicit RemoveFileCommand(search_server::SearchServer& server) : server_(server){}
    void execute(const std::wstring& path) override {
        LogFile::getWatcher().write(L"[RemoveFileCommand] execute path=" + path);
        server_.removeFileFromIndex(path);
    }
private:
    search_server::SearchServer& server_;
};



#endif //SEARCHENGINE_INDEXCOMMANDS_H
