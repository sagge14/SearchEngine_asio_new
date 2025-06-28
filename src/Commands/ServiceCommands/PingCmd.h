//
// Created by Sg on 05.06.2024.
//

#ifndef SEARCHENGINE_PINGCMD_H
#define SEARCHENGINE_PINGCMD_H
#include "Commands/Command.h"


class PingCmd : public Command {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};


#endif //SEARCHENGINE_PINGCMD_H
