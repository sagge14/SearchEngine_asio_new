//
// Created by Sg on 06.06.2024.
//

#include "GetMessageCmd.h"
#include "MessageQueue.h"
std::vector<uint8_t> GetMessageCmd::execute(const std::vector<uint8_t> &_data) {
    uint32_t  user_id = 1;
    return MessageQueue::getInstance().getMessage(user_id);
}