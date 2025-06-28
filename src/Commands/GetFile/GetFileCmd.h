//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_GETFILECMD_H
#define SEARCHENGINE_TEST_GETFILECMD_H
#include "Commands/Command.h"
#include <functional>
#include <utility>

class GetFileCmd : public Command {

    std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> action;

public:
    explicit GetFileCmd(std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> func) : action{std::move(func)} {};
    static std::vector<uint8_t> downloadFileByPath(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> downloadFileByPath(const std::string& data);
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};

#endif //SEARCHENGINE_TEST_GETFILECMD_H
