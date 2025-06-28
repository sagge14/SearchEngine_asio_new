//
// Created by Sg on 29.09.2024.
//

#ifndef SEARCHENGINE_GETTELEGAWAYCMD_H
#define SEARCHENGINE_GETTELEGAWAYCMD_H
#include "Commands/Command.h"
#include "Commands/GetJsonTelega/Telega.h"

class GetTelegaWayCmd : public Command {
protected:
    std::vector<uint8_t>  GetTelegaWay(const std::vector<uint8_t> & _data, Telega::TYPE _type);
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override = 0;
};

class GetTelegaWayVhCmd : public GetTelegaWayCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};

class GetTelegaWayIshCmd : public GetTelegaWayCmd {
public:
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;
};

#endif //SEARCHENGINE_GETTELEGAWAYCMD_H
