//
// Created by Sg on 05.06.2024.
//

#include "PingCmd.h"

std::vector<uint8_t> PingCmd::execute(const std::vector<uint8_t>& _data)
{
    auto result = executeResult(_data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{0};
}

command_execution::CommandResult PingCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    const std::vector<uint8_t> pingMessage = { 'P', 'I', 'N', 'G' };
    const std::vector<uint8_t> pongMessage = { 'P', 'O', 'N', 'G' };

    if (data == pingMessage)
        return command_execution::CommandResult::success(pongMessage);

    return command_execution::CommandResult::failure(
        command_execution::ErrorCode::InvalidRequest,
        "PING expects exactly four ASCII bytes: PING");
}
