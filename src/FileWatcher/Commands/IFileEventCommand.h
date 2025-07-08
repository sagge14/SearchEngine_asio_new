//
// Created by Sg on 06.07.2025.
//
#include <string>
#ifndef SEARCHENGINE_IFILEEVENTCOMMAND_H
#define SEARCHENGINE_IFILEEVENTCOMMAND_H
class IFileEventCommand {
public:
    virtual ~IFileEventCommand() = default;
    virtual void execute(const std::wstring& path) = 0;
};

#endif //SEARCHENGINE_IFILEEVENTCOMMAND_H
