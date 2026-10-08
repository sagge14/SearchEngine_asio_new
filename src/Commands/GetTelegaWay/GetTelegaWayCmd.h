//
// Created by Sg on 29.09.2024.
//

#ifndef SEARCHENGINE_GETTELEGAWAYCMD_H
#define SEARCHENGINE_GETTELEGAWAYCMD_H
#include "Commands/Command.h"
#include "Commands/GetJsonTelega/Telega.h"

class GetTelegaWayCmd : public Command {
protected:
    [[nodiscard]] static std::vector<uint8_t> GetTelegaWay(
        const std::vector<uint8_t>& _data,
        Telega::TYPE _type);
    [[nodiscard]] static command_execution::CommandResult GetTelegaWayResult(
        const std::vector<uint8_t>& data,
        Telega::TYPE type);
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override = 0;
};

class GetTelegaWayVhCmd : public GetTelegaWayCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

class GetTelegaWayIshCmd : public GetTelegaWayCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

#endif //SEARCHENGINE_GETTELEGAWAYCMD_H
