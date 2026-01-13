//
// Created by Sg on 08.09.2025.
//

#ifndef SEARCHENGINE_GETTELEGASINGLEATTACHMENTCMD_H
#define SEARCHENGINE_GETTELEGASINGLEATTACHMENTCMD_H

#include "Commands/Command.h"


class GetTelegaSingleAttachmentCmd : public Command {

public:

    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;

};


#endif //SEARCHENGINE_GETTELEGASINGLEATTACHMENTCMD_H
