#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H

#include <unordered_map>
#include <queue>
#include <string>
#include <mutex>
#include "Message.h"


class MessageQueue {

public:

    static MessageQueue& getInstance();

    void addMessage(uint32_t user_id, const std::vector<uint8_t>& bytes);
    std::vector<uint8_t> getMessage(uint32_t user_id);

private:
    MessageQueue();
    void loadMessagesFromDisk();

    // Non-copyable and non-movable
    MessageQueue(const MessageQueue&) = delete;
    MessageQueue& operator=(const MessageQueue&) = delete;
    MessageQueue(MessageQueue&&) = delete;
    MessageQueue& operator=(MessageQueue&&) = delete;

    std::unordered_map<uint32_t, std::queue<std::string>> userMessages_;
    unsigned long messageCounter_;
    std::mutex mutex_;
};

#endif // MESSAGEQUEUE_H
