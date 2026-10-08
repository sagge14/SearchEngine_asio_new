#pragma once

#include "Commands/CommandResult.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "JSON/ConverterJSON.h"

[[nodiscard]] inline command_execution::CommandResult mapAutoPadSourceError(
    const Telega::SourceError& error)
{
    using command_execution::ErrorCode;
    if (error.availability == Telega::SourceAvailability::Disabled) {
        return command_execution::CommandResult::failure(
            ErrorCode::DataSourceDisabled,
            error.what());
    }
    return command_execution::CommandResult::failure(
        ErrorCode::DataSourceUnavailable,
        error.what());
}

[[nodiscard]] command_execution::CommandResult buildTelegiJsonFromSearchHits(
    const listAnswer& results);
