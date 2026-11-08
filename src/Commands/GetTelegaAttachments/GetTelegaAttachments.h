//
// Created by Sg on 31.08.2025.
//

#pragma once
#include "Commands/Command.h"
#include "Commands/GetJsonTelega/Telega.h"

class GetTelegaAttachmentsCmd : public Command {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& _data) override;
    [[nodiscard]] command_execution::CommandResult executeResult(
        const std::vector<uint8_t>& data) override;
};



