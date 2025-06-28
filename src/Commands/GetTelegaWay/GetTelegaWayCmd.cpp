//
// Created by Sg on 29.09.2024.
//

#include "GetTelegaWayCmd.h"
#include "nlohmann/json.hpp"
#include "TelegaWay.h"
std::vector<uint8_t> GetTelegaWayCmd:: GetTelegaWay(const std::vector<uint8_t> &_data, Telega::TYPE _type) {

    namespace nh = nlohmann;
    std::string request(_data.begin(), _data.end());
    nh::json jsonTelegi;
    jsonTelegi = TelegaWay{request,_type}.result_way;
    std::string jsonString = jsonTelegi.dump();

    return std::vector<uint8_t>(jsonString.begin(), jsonString.end());
}

std::vector<uint8_t> GetTelegaWayVhCmd::execute(const std::vector<uint8_t> &data) {
    return GetTelegaWay(data, Telega::TYPE::VHOD);
}

std::vector<uint8_t> GetTelegaWayIshCmd::execute(const std::vector<uint8_t> &data) {
    return GetTelegaWay(data, Telega::TYPE::ISHOD);
}
