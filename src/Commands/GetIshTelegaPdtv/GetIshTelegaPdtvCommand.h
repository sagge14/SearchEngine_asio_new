//
// Created by Sg on 25.08.2025.
//

#pragma  once
#include "Commands/Command.h"
#include "Commands/GetJsonTelega/Telega.h"

class GetIshTelegaPdtvCommand : public Command {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& _data) override;
};

