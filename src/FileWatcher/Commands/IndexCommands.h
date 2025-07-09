//
// Created by Sg on 06.07.2025.
//

#ifndef SEARCHENGINE_INDEXCOMMANDS_H
#define SEARCHENGINE_INDEXCOMMANDS_H
#include "IFileEventCommand.h"
#include "SearchServer/SearchServer.h"

class AddFileCommand : public IFileEventCommand {
public:
    explicit AddFileCommand(search_server::SearchServer& server) : server_(server) {}
    void execute(const std::wstring& path) override {
        server_.addFileToIndex(path);
    }
private:
    search_server::SearchServer& server_;
};

class RemoveFileCommand : public IFileEventCommand {
public:
    explicit RemoveFileCommand(search_server::SearchServer& server) : server_(server){}
    void execute(const std::wstring& path) override {
        server_.removeFileFromIndex(path);
    }
private:
    search_server::SearchServer& server_;
};



#endif //SEARCHENGINE_INDEXCOMMANDS_H
