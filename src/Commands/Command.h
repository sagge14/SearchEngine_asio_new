//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_COMMAND_H
#define SEARCHENGINE_TEST_COMMAND_H
#include <vector>
#include <cstdint>
#include <iostream>
class Command {
protected:
  //  bool log = false;
public:
  //  void doLogg(bool _log) {log =_log;}
    virtual ~Command() = default;
    virtual std::vector<uint8_t> execute(const std::vector<uint8_t>& data) = 0;
    virtual void log() {};
};




#endif //SEARCHENGINE_TEST_COMMAND_H
