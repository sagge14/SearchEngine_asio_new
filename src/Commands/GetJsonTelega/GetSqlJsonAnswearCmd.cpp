#include "GetJsonTelegaCmd.h"

#include <string>
#include <utility>

std::vector<uint8_t> GetSqlJsonAnswearCmd::execute(const std::vector<uint8_t>& data)
{
    auto result = executeResult(data);
    return result.succeeded()
        ? std::move(result.payload)
        : std::vector<uint8_t>{};
}

command_execution::CommandResult GetSqlJsonAnswearCmd::executeResult(
    const std::vector<uint8_t>& data)
{
    if (server_->getIsUpdateRunning())
    {
        const std::string update = "update";
        return command_execution::CommandResult::success(
            {update.begin(), update.end()});
    }

    const std::string request(data.begin(), data.end());
    return buildTelegiJsonFromSearchHits(server_->getAnswer(request));
}
