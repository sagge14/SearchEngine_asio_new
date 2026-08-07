//
// Created by Sg on 06.07.2025.
//

#ifndef SEARCHENGINE_INDEXCOMMANDS_H
#define SEARCHENGINE_INDEXCOMMANDS_H
#include "IFileEventCommand.h"
#include "UpdateOpisBaseCommand.h"
#include "SearchServer/SearchServer.h"
#include "MyUtils/LogFile.h"

template<typename TaskID>
class PeriodicTaskManager;

template<typename TaskID>
class AddFileCommand : public IFileEventCommand {
public:

    AddFileCommand(search_server::SearchServer& server, PeriodicTaskManager<TaskID>& ptm,
                   const std::vector<std::string>& ext,
                   bool enablePrmShortContentAutodetect = true)
        : server_(server)
    {
            if (enablePrmShortContentAutodetect) {
                opis_command_ =
                    std::make_unique<UpdateOpisBaseCommand<TaskID>>(ptm, ext);
            }
    }

    void execute(const std::wstring& path) override {
        LogFile::getWatcher().write(L"[AddFileCommand] execute path=" + path);

        server_.addFileToIndex(path);

        if (opis_command_) {
            try {
                opis_command_->execute(path);
            } catch (const std::exception& e) {
                LogFile::getWatcher().write(std::string("[execute] opis_command_ exception: ") + e.what());
            } catch (...) {
                LogFile::getWatcher().write("[execute] opis_command_ unknown exception!");
            }
        }



    }


private:

    search_server::SearchServer& server_;
    std::unique_ptr<UpdateOpisBaseCommand<TaskID>> opis_command_;
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
