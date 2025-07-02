//
// Created by user on 09.09.2023.
//

#include "Telega.h"
#include "SQLite/mySql.h"
#include <codecvt>
#include <string>
#include <regex>
#include <optional>

namespace conv2
{
    using namespace std;
    using convert_t = std::codecvt_utf8<wchar_t>;

    wstring_convert<convert_t, wchar_t> strconverter;

    string to_string(const std::wstring& wstr)                                 //Конструкция перевода в String из WString
    {
        return strconverter.to_bytes(wstr);
    }

    wstring to_wstring(const std::string& str)                                 //Конструкция перевода в WString из String
    {
        return strconverter.from_bytes(str);
    }
}



Telega::Telega(const std::map<std::string, std::string>& _record, TYPE _type, float _rel) : rel{_rel}, type{_type} {


    initTelega( _record);

}

std::vector<std::string> Telega::getBases(const std::string &_dir) {
    {
        /**
        Функция получения всех файлов из дирректории @param dir и ее подпапках, исключая имена папок */

        namespace fs = std::filesystem;

        auto recursiveGetFileNamesByExtension = [](const std::string &path)
        {
            std::vector<std::string> _docPaths;
            try{
            for(auto& p: fs::recursive_directory_iterator(path))
                if(p.is_regular_file() && ((p.path().filename().string().find('-') == 2 && !p.path().extension().compare(".db3")) ||  p.path().filename() == "ARCHIVE.db3")
                        )
                {_docPaths.push_back(p.path().string());}
            return _docPaths;
            }
            catch(...)
            {
                std::cout<<"Error in telega dir" << std::endl;
                return _docPaths;
            }
        };

        return std::move(recursiveGetFileNamesByExtension((_dir)));
    };
}

std::list<std::map<std::string,std::string>> Telega::findBase(const std::string& _condition, TYPE _type, bool single) {

    std::string bases_dir, isp_podp_key, kr_key, from_to_key, blank_key;
    const decltype(getBases(TYPE{})) *base;
    std::list<std::map<std::string,std::string>> result{};



    auto extractLimit = [](const std::string& cond) -> std::optional<int> {
        // ищем слово limit и одну или более цифр после него
        static const std::regex re(R"(\blimit\s+(\d+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(cond, m, re)) {
            try {
                return std::stoi(m[1].str());
            } catch (...) {
                // на случай overflow
                return std::nullopt;
            }
        }
        return std::nullopt;
    };

    auto condition = "where " + _condition;

    if (_type== TYPE::VHOD)
        {
        isp_podp_key = "Familia"; base = &Telega::b_prm;
        kr_key = "PrilName1"; from_to_key= "FFrom";
        blank_key = "Copyes2";
        }
    if (_type == TYPE::ISHOD)
        {isp_podp_key = "Copyes";  base = &Telega::b_prd;  kr_key = "FFrom5";
            blank_key = "Blank";
            from_to_key = "FFrom1";}


    auto limitOpt = extractLimit(condition);
    int collected = 0;

    try {
        for (const auto &base_name: *base) {

            std::cout << base_name << std::endl;
            std::cout << condition << std::endl;
            auto sql_qry = "select `index`, ddata, " + from_to_key + ", TelNo, PodpNo, DataPodp, " +
                           isp_podp_key + ", " + kr_key + ", PrilName, KolPril, DirectTo, FileName, " + blank_key +", Edit, GdeSHT from ARCHIVE " +
                           condition;
            SQL_INST.excSql(base_name, sql_qry);
          //  std::cout << "records :" << SQL_INST.size() << std::endl;
            if (!SQL_EMPTY) {
                for (const auto& row : SQL_INST) {
                    result.push_back(row);
                    ++collected;
                    if (limitOpt && collected >= *limitOpt)
                        return result;
                    if (single)
                        return result;
                }
            }
        }
    }
    catch(...)
    {std::cout << "sql error" << std::endl;}

    return std::move(result);
}

std::vector<std::string> Telega::getBases(Telega::TYPE _type) {

        auto getYearNow = [] (){
        char dataTime[20];
        time_t now = std::time(nullptr);
        strftime(dataTime, sizeof(dataTime), "%Y", localtime(&now));
        return std::string{dataTime};
    };

    std::vector<std::string> res;
    std::string bases_dir;
    static const std:: string ms[13] = {"01","02","03","04","05","06","07","08","09","10","11","12","13"};

    switch (_type)
    {
        case TYPE::VHOD:  { bases_dir = prm_base_dir;   break; }
        case TYPE::ISHOD: { bases_dir = prd_base_dir;   break; }
        case TYPE::NOTTG:
            break;
    }

    if(getYearNow() == Telega::year)
        res.push_back(bases_dir + "\\ARCHIVE.db3");

    for(int i = 12; i >= 0; i--)
    {
        auto base_name = bases_dir + "\\METH_BASES\\" + ms[i] + "-" + Telega::year + ".db3";
    //    std::cout << base_name << std::endl;
        if (!std::filesystem::exists(base_name))
        {
    //        std::cout << base_name << " - not exist -" << std::endl;
            continue;
        }
        res.push_back(base_name);
    //    std::cout << base_name << std::endl;
    }

    if(res.empty() && getYearNow() != Telega::year)
        res.push_back(bases_dir + "ARCHIVE.db3");

    return res;
}

Telega::TYPE Telega::getTypeFromDir(const std::filesystem::path& _file_dir) {
    return _file_dir.extension() == "" ? TYPE::VHOD : TYPE::ISHOD;
}

void Telega::initTelega(const std::map<std::string, std::string> &_record) {
    try
    {

        if(type != TYPE::NOTTG)
        {
            if(dir.empty())
                dir = _record.at("DirectTo") + _record.at("FileName");
            num = _record.at("Index");
            from_to = type == TYPE::VHOD ? _record.at("FFrom") : _record.at("FFrom1");
            date = _record.at("DData");
            date_podp = _record.at("DataPodp");
            podp_num = _record.at("PodpNo");
            pril_name = _record.at("PrilName");
            pril_count = _record.at("KolPril");
            isp = type == TYPE::VHOD ? _record.at("Familia") : _record.at("Copyes");
            //auto ff = conv2::to_wstring(isp);
            //auto gggg = conv2::to_string(ff);
            kr = type == TYPE::VHOD ? _record.at("PrilName1") : _record.at("FFrom5");
            tel_num = type == TYPE::VHOD ? _record.at("TelNo") : num; ;
            blank = type ==  TYPE::VHOD ? _record.at("Copyes2") : _record.at("Blank"); ;
            last_mesto = _record.at("Edit");
            gde_sht = _record.at("GdeSHT");
            try
            {
                auto tt = conv2::to_wstring(from_to);
            }
            catch(...)
            {
                podp_num = "";
                from_to = "";
                isp = "";
            }
        }

    }
    catch (...){
        std::cout << " --- Error " << dir << std::endl;
    }
}

std::string Telega::getNumFromFileName(const std::filesystem::path& path)
{
    auto fn = path.filename().string();
    return fn.substr(0,fn.find_last_of('.') > 0 ? fn.find_last_of('.') : fn.length());

}

Telega::Telega(const std::filesystem::path& _p, float _rel) : Telega(TYPE::NOTTG, _p, _rel) {

    type = getTypeFromDir(_p);

    auto res = findBase("`index` = "  + getNumFromFileName(_p), type, true);

    if(!res.empty())
        initTelega(res.front());
}
