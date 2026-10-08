//
// Created by Sg on 29.10.2024.
//

#ifndef SEARCHENGINE_GETATTACHMENTSCMD_H
#define SEARCHENGINE_GETATTACHMENTSCMD_H

#include "Commands/Command.h"
#include "Commands/GetAttachments/AttachmentPackage.h"

class GetAttachmentsCmd : public Command {
    static void deleteDirectory(const std::filesystem::path& dirPath);
    std::filesystem::path configPath_;
public:
    explicit GetAttachmentsCmd(
        std::filesystem::path configPath = "prefix_map.json");
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};


#endif //SEARCHENGINE_GETATTACHMENTSCMD_H
