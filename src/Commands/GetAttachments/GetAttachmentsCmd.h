//
// Created by Sg on 29.10.2024.
//

#ifndef SEARCHENGINE_GETATTACHMENTSCMD_H
#define SEARCHENGINE_GETATTACHMENTSCMD_H

#include "Commands/Command.h"
#include "Commands/SaveMessage/Message.h"

class GetAttachmentsCmd : public Command {
    void deleteDirectory(const std::filesystem::path& dirPath);
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};


#endif //SEARCHENGINE_GETATTACHMENTSCMD_H
