//
// Created by Sg on 06.06.2024.
//

#ifndef SEARCHENGINE_GETMESSAGECMD_H
#define SEARCHENGINE_GETMESSAGECMD_H
#include "Commands/Command.h"

class GetMessageCmd : public Command {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};


#endif //SEARCHENGINE_GETMESSAGECMD_H
