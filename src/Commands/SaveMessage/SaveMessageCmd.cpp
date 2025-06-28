//
// Created by Sg on 01.06.2024.
//

#include "SaveMessageCmd.h"
#include "MessageQueue.h"
std::vector<uint8_t> SaveMessageCmd::execute(const std::vector<uint8_t> &_data) {
    MessageQueue::getInstance().addMessage(user_id_, _data);
    return {1};
}
