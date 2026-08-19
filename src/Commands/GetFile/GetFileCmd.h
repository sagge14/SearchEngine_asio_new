//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_GETFILECMD_H
#define SEARCHENGINE_TEST_GETFILECMD_H
#include "Commands/Command.h"
#include <filesystem>
#include <functional>
#include <utility>

class GetFileCmd : public Command {

    using ResultAction = std::function<command_execution::CommandResult(
        const std::vector<uint8_t>&)>;

    ResultAction action;

public:
    explicit GetFileCmd(ResultAction func) : action{std::move(func)} {}
    static std::vector<uint8_t> downloadFileByPath(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> downloadFileByPath(const std::string& data);
    [[nodiscard]] static command_execution::CommandResult downloadFileResultByPath(
        const std::vector<uint8_t>& data);
    [[nodiscard]] static command_execution::CommandResult downloadFileResultByPath(
        const std::string& data);
    [[nodiscard]] static command_execution::CommandResult downloadFileResultByPath(
        const std::filesystem::path& filePath);
    [[nodiscard]] static command_execution::CommandResult rejectRawBinFileDownload(
        const std::vector<uint8_t>& requestData);
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

#endif //SEARCHENGINE_TEST_GETFILECMD_H
