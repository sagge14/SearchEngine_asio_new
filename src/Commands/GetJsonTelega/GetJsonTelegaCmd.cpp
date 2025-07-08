//
// Created by Sg on 23.05.2024.
//

#include "GetJsonTelegaCmd.h"

std::vector<uint8_t> GetJsonTelegaCmd::getSqlJsonTelega(const std::vector<uint8_t>& _data, Telega::TYPE _type) {


    std::string request(_data.begin(), _data.end());

    try {
        Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
        Telega::b_prd = Telega::getBases(Telega::TYPE::ISHOD);
    }
    catch (...)
    {std::cout << "sql base error" << std::endl;}

    std::vector<Telega> telegi;
    nh::json jsonTelegi;

    try {

        auto res = Telega::findBase(request, _type);
        telegi.reserve(res.size());

        for (const auto& r: res)
            telegi.emplace_back(r, _type);

        jsonTelegi = telegi;
    }
    catch (...)
    {std::cout << "sql answer error" << std::endl;}

    std::string jsonString = jsonTelegi.dump();

    return std::vector<uint8_t>(jsonString.begin(), jsonString.end());
}

std::vector<uint8_t> GetJsonTelegaVhCmd::execute(const std::vector<uint8_t>& _data)
{
    return getSqlJsonTelega(_data, Telega::TYPE::VHOD);
}
std::vector<uint8_t> GetJsonTelegaIshCmd::execute(const std::vector<uint8_t>& _data)
{
    return getSqlJsonTelega(_data, Telega::TYPE::ISHOD);
}
std::vector<uint8_t> GetSqlJsonAnswearCmd::execute(const std::vector<uint8_t>& _data) {


    if(server_->getIsUpdateRunning())
    {
        std::string update = "update";
        return std::vector<uint8_t>(update.begin(), update.end());
    }


    std::string request(_data.begin(), _data.end());

    std::set<std::pair<std::string, Telega::TYPE>> set_num_type;

    std::list<std::pair<std::string, float>> results = server_->getAnswer(request);

    std::vector<Telega> telegi;
    telegi.reserve(results.size());

    Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
    Telega::b_prd = Telega::getBases(Telega::TYPE::ISHOD);

    for(const auto& [std_str_file_path,rel]:results)
    {
        auto fs_path = std::filesystem::path{std_str_file_path};
        auto num = Telega::getNumFromFileName(fs_path);

        if(num.empty())
            continue;

        auto type= Telega::getTypeFromDir(fs_path);

        if((set_num_type.insert({num,type}).second))
            telegi.emplace_back(fs_path,rel);
    }


    nh::json jsonTelegi = telegi;

    std::string jsonString = jsonTelegi.dump();

    return std::vector<uint8_t>(jsonString.begin(), jsonString.end());
}