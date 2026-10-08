//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_GETJSONTELEGACMD_H
#define SEARCHENGINE_TEST_GETJSONTELEGACMD_H

#include "AutoPadSource.h"
#include "Commands/Command.h"
#include "Commands/CommandResult.h"
#include "SearchServer/SearchServer.h"
#include "Telega.h"

class GetJsonTelegaCmd : public Command {
protected:
    [[nodiscard]] static command_execution::CommandResult getSqlJsonTelegaResult(
        const std::vector<uint8_t>& data,
        Telega::TYPE type);

    std::vector<uint8_t> getSqlJsonTelega(
        const std::vector<uint8_t>& data,
        Telega::TYPE type);
public:
    virtual std::vector<uint8_t> execute(const std::vector<uint8_t>& data)  = 0;
};

class GetJsonTelegaVhCmd : public GetJsonTelegaCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

class GetJsonTelegaIshCmd : public GetJsonTelegaCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

class GetSqlJsonAnswearCmd : public Command {
    search_server::SearchServer* server_;
public:
    explicit GetSqlJsonAnswearCmd(search_server::SearchServer* server) : server_{server}{};
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};

#endif //SEARCHENGINE_TEST_GETJSONTELEGACMD_H
