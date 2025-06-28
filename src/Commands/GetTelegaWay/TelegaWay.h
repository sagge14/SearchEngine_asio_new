//
// Created by Sg on 29.09.2024.
//

#ifndef SEARCHENGINE_TELEGAWAY_H
#define SEARCHENGINE_TELEGAWAY_H
#include <string>
#include "Commands/GetJsonTelega/Telega.h"

struct TelegaWay {

    static inline std::string base_way_dir = {};
    static inline std::string base_f12_dir = {};
    static inline std::string work_year = {};
    Telega::TYPE type;

    [[maybe_unused]] TelegaWay(const std::string& num, Telega::TYPE _type);
    std::list<std::map<std::string,std::string>> result_way{};

};


#endif //SEARCHENGINE_TELEGAWAY_H
