//
// Created by Sg on 01.06.2024.
//

#ifndef SEARCHENGINE_SAVEMESSAGECMD_H
#define SEARCHENGINE_SAVEMESSAGECMD_H
#include "Commands/Command.h"

class SaveMessageCmd : public Command {
    uint32_t  user_id_;
public:
    explicit SaveMessageCmd(uint32_t  _user_id) : user_id_{_user_id}{};
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};


#endif //SEARCHENGINE_SAVEMESSAGECMD_H
