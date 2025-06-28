#include "MessageQueue.h"
#include <fstream>
#include <iostream>
#include <filesystem>

MessageQueue& MessageQueue::getInstance() {
    static MessageQueue instance;
    return instance;
}

MessageQueue::MessageQueue() : messageCounter_(0) {
    loadMessagesFromDisk();
}

void MessageQueue::addMessage(uint32_t user_id, const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::path messagesDir = std::filesystem::current_path() / "messages";
    auto userDir = messagesDir / std::to_string(user_id);

    std::filesystem::create_directories(userDir);

    std::string filePath = (userDir / std::to_string(++messageCounter_)).string() + ".msg";

    std::ofstream outFile(filePath, std::ios::binary);
    if (outFile.is_open()) {
        outFile.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        userMessages_[user_id].push(filePath);
    } else {
        std::cerr << "Failed to open file: " << filePath << std::endl;
    }
}

std::vector<uint8_t> MessageQueue::getMessage(uint32_t _user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (userMessages_.find(_user_id) != userMessages_.end() && !userMessages_[_user_id].empty()) {
        std::string filePath = userMessages_[_user_id].front();
        userMessages_[_user_id].pop();

        std::ifstream inFile(filePath, std::ios::binary);
        if (!inFile) {
            std::cerr << "Error: Unable to open file " << filePath << std::endl;
            return {};
        }

        // Перемещаем указатель чтения в конец файла, чтобы получить его размер
        inFile.seekg(0, std::ios::end);
        std::streampos fileSize = inFile.tellg();
        inFile.seekg(0, std::ios::beg);

        // Читаем данные в вектор
        std::vector<uint8_t> message_bytes(static_cast<size_t>(fileSize));
        if (inFile.read(reinterpret_cast<char*>(message_bytes.data()), message_bytes.size())) {
            inFile.close();
            return message_bytes;
        }

        inFile.close();
        std::cerr << "Error: Unable to read file " << filePath << std::endl;
        return {};
    }

    return {};
}


void MessageQueue::loadMessagesFromDisk() {
    std::filesystem::path messagesDir = std::filesystem::current_path() / "messages";

    // Проверка существования папки "messages"
    if (!std::filesystem::exists(messagesDir)) {
        std::filesystem::create_directories(messagesDir);
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(messagesDir)) {
        if (entry.is_directory()) {
            uint32_t user_id;
            try {
                user_id = std::stoul(entry.path().filename().string());
                std::cout << "User ID: " << user_id << std::endl;
            } catch (const std::invalid_argument& e) {
                std::cerr << "Invalid argument: " << e.what() << std::endl;
                continue;
            } catch (const std::out_of_range& e) {
                std::cerr << "Out of range: " << e.what() << std::endl;
                continue;
            }

            for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
                if (file.path().extension() == ".msg") {
                    userMessages_[user_id].push(file.path().string());
                }
            }
        }
    }
}
