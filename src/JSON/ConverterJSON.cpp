//
// Created by user on 04.02.2023.
//
#include <unordered_map>
#include <vector>
#include <string>
#include <filesystem>

#include "ConverterJSON.h"
#include "SearchServer/SearchServer.h"
#include "scheduler/BackupTask.h"
#include "Commands/GetAttachments/PrefixMap.h"
#include "Commands/GetJsonTelega/Telega.h"

#include "MyUtils/Encoding.h"


#define TO_JSON(J, S) J[#S] = val.S;
#define FROM_JSON(J, S) J.at(#S).get_to(val.S);

void ConverterJSON::setSettings(const search_server::Settings &val) {
    /**
    Функция записи настроек сервера в json файл. */

    nh::json jsonSettings;

    jsonSettings["config"]["Name"] = val.name;
    jsonSettings["config"]["Version"] = val.version;
    jsonSettings["config"]["max_response"] = val.maxResponse;
    jsonSettings["config"]["thread_count"] = val.threadCount;
    jsonSettings["config"]["dir"] = val.dir;
    jsonSettings["config"]["asio_port"] = val.port;
    jsonSettings["config"]["ind_time"] = val.indTime;
    jsonSettings["config"]["text_request"] = val.requestText;
    jsonSettings["config"]["exact_search"] = val.exactSearch;
    jsonSettings["config"]["dirs"] = val.dirs;
    jsonSettings["config"]["extensions"] = val.extensions;
    jsonSettings["Files"] = val.files;
    jsonSettings["config"]["exclude_dirs"] = val.excludeDirs;
    jsonSettings["config"]["prm_base_dir"] = val.prm_base_dir;
    jsonSettings["config"]["prd_base_dir"] = val.prd_base_dir;

    std::ofstream jsonFileSettings("Settings.json");
    jsonFileSettings << jsonSettings;
    jsonFileSettings.close();

}

search_server::Settings ConverterJSON::getSettings(const std::string& jsonPath) {
    /**
    Функция получения настроек сервера из json файла. */

    search_server::Settings s;
    nh::json jsonSettings;
    std::ifstream jsonFileSettings(jsonPath);

    try
    {
        jsonSettings = nh::json::parse(jsonFileSettings);
        jsonFileSettings.close();

        jsonSettings.at("config").at("Name").get_to(s.name);
        jsonSettings.at("config").at("Version").get_to(s.version);
        jsonSettings.at("config").at("max_response").get_to(s.maxResponse);
        jsonSettings.at("config").at("thread_count").get_to(s.threadCount);
        jsonSettings.at("config").at("port").get_to(s.port);
        jsonSettings.at("config").at("dir").get_to(s.dir);
        jsonSettings.at("config").at("exact_search").get_to(s.exactSearch);
        jsonSettings.at("config").at("hide_mode").get_to(s.hideMode);
        jsonSettings.at("config").at("ind_time").get_to(s.indTime);
        jsonSettings.at("config").at("dirs").get_to(s.dirs);
        jsonSettings.at("config").at("extensions").get_to(s.extensions);
        jsonSettings.at("config").at("year").get_to(s.year);
        jsonSettings.at("config").at("text_request").get_to(s.requestText);
        jsonSettings.at("config").at("exclude_dirs").get_to(s.excludeDirs);
        jsonSettings.at("config").at("prm_base_dir").get_to(s.prm_base_dir);
        jsonSettings.at("config").at("prd_base_dir").get_to(s.prd_base_dir);

        jsonSettings.at("Files").get_to(s.files);

    }
    catch (nh::json::parse_error& ex)
    {
        std::cerr << "JSON parse error at byte " << ex.byte << std::endl;
        throw myExp(jsonPath);
    }
    catch (...)
    {
        throw myExp(jsonPath);
    }

    return s;
}

std::vector<std::string> ConverterJSON::getRequests(const std::string& jsonPath) {
    /**
    функция получения запросов из json файла, адресованных поисковому серверу.
    Если json файла - некорректен или отсутствует возвращается пустой вектор,
    работа сервера не будет прекращена. */

    std::vector<std::string> requests;
    nh::json jsonRequests;
    std::ifstream jsonFileRequests(jsonPath);

    try
    {
        jsonRequests = nh::json::parse(jsonFileRequests);
        jsonFileRequests.close();
    }
    catch (nh::json::parse_error& ex)
    {
        jsonFileRequests.close();
        return std::move(requests);
    }
    catch (...)
    {
        jsonFileRequests.close();
        return std::move(requests);
    }

    jsonRequests.at("Requests").get_to(requests);

    return requests;
}

std::string ConverterJSON::putAnswers(const listAnswers& answers, const std::string& jsonPath) {
    /**
     функция записи ответов сервера на запросы в json файл. */

    nh::json jsonAnswers;

    for(const auto& answer: answers)
    {
        if(!answer.first.empty())
        {
            jsonAnswers["Answers"][answer.second]["result"] = true;
            jsonAnswers["Answers"][answer.second]["relevance"] = answer.first;
        }
        else
            jsonAnswers["Answers"][answer.second]["result"] = false;
    }

    std::ofstream jsonFileSettings(jsonPath);

    jsonFileSettings << std::setw(2) << jsonAnswers;
    std::string ss = nh::to_string(jsonAnswers);
    jsonFileSettings.close();
    return move(ss);
}

std::vector<BackupGroup> ConverterJSON::parseBackupJobs(const std::string& path) {

    std::ifstream file(path);
    std::vector<BackupGroup> groups;
    nh::json jsonData;
    try {
        jsonData = nh::json::parse(file);
    } catch (...) {
        return groups;
    }

    if (!jsonData.contains("BackupJobs") || !jsonData["BackupJobs"].is_array())
        return groups;

    for (const auto& groupJson : jsonData["BackupJobs"]) {
        try {
            BackupGroup group;
            group.backup_dir = groupJson.at("backup_dir").get<std::string>();
            group.period_sec = groupJson.value("period_sec", 3600);
            if (!groupJson.contains("targets") || !groupJson["targets"].is_array())
                continue;

            for (const auto& entry : groupJson["targets"]) {
                BackupTarget t;
                t.src = entry.at("src").get<std::string>();
                t.max_versions = entry.value("max_versions", 5);
                t.is_directory = entry.value("is_directory", false);
                group.targets.push_back(std::move(t));
            }
            if (!group.targets.empty())
                groups.push_back(std::move(group));
        } catch (...) {
            continue;
        }
    }
    return groups;
}


std::vector<std::string> ConverterJSON::getRequestsFromString(const std::string &jsonString) {

    std::vector<std::string> requests;
    nh::json jsonRequests;

    try
    {
        jsonRequests = nh::json::parse(jsonString);
    }
    catch (nh::json::parse_error& ex)
    {
        return std::move(requests);
    }
    catch (...)
    {
        return std::move(requests);
    }

    jsonRequests.at("Requests").get_to(requests);

    return requests;
}

std::string ConverterJSON::getJsonTelega(const Telega &val) {

    nh::json jsonTelega;

    TO_JSON(jsonTelega, num)
    TO_JSON(jsonTelega, type)
    TO_JSON(jsonTelega, from_to)
    TO_JSON(jsonTelega, isp)
    TO_JSON(jsonTelega, podp_num)
    TO_JSON(jsonTelega, date)
    TO_JSON(jsonTelega, date_podp)
    TO_JSON(jsonTelega, dir)
    TO_JSON(jsonTelega, rel)
    TO_JSON(jsonTelega, kr)
    TO_JSON(jsonTelega, pril_name)
    TO_JSON(jsonTelega, pril_count)

    return to_string(jsonTelega);
}

PrefixMap ConverterJSON::loadAttachPrefixLogin(const std::filesystem::path &path) {

    PrefixMap pm;
    std::ifstream file(path);
    nh::json jsonFile;

    std::string str_prefix;
    std::map<std::string, std::string> str_map;

    jsonFile = nh::json::parse(file);

    jsonFile.at("prefix").get_to(str_prefix);
    jsonFile.at("map").get_to(str_map);

    pm.prefix = encoding::utf8_to_wstring(str_prefix);

    for(const auto& [key,value]:str_map)
    {
        pm.map_.insert({encoding::utf8_to_wstring(key),encoding::utf8_to_wstring(value)});
    }

    return pm;
}
void ConverterJSON::saveAttachPrefixLogin(const PrefixMap& pm, const std::filesystem::path& path) {
    nh::json jsonFile;


    // Преобразуем поле prefix обратно в строку
    std::string str_prefix = encoding::wstring_to_utf8(pm.prefix);
    jsonFile["prefix"] = str_prefix;

    // Преобразуем карту map_ обратно в строковый формат
    std::map<std::string, std::string> str_map;
    for (const auto& [key, value] : pm.map_) {
        str_map[encoding::wstring_to_utf8(key)] = encoding::wstring_to_utf8(value);
    }
    jsonFile["map"] = str_map;

    // Сохраняем JSON в файл
    std::ofstream file(path, std::ios::out | std::ios::binary);
    file << jsonFile.dump(4); // С форматированием для удобного чтения (4 отступа)
}



void to_json(nh::json& jsonTelega, const Telega &val)
{
    TO_JSON(jsonTelega, num)
    TO_JSON(jsonTelega, type)
    TO_JSON(jsonTelega, from_to)
    TO_JSON(jsonTelega, isp)
    TO_JSON(jsonTelega, podp_num)
    TO_JSON(jsonTelega, date)
    TO_JSON(jsonTelega, date_podp)
    TO_JSON(jsonTelega, dir)
    TO_JSON(jsonTelega, rel)
    TO_JSON(jsonTelega, tel_num)
    TO_JSON(jsonTelega, kr)
    TO_JSON(jsonTelega, pril_name)
    TO_JSON(jsonTelega, pril_count)
    TO_JSON(jsonTelega, blank)
    TO_JSON(jsonTelega, gde_sht)
    TO_JSON(jsonTelega, last_mesto)
}
void from_json(const nh::json& jsonTelega, Telega &val)
{
    FROM_JSON(jsonTelega, num)
    FROM_JSON(jsonTelega, type)
    FROM_JSON(jsonTelega, from_to)
    FROM_JSON(jsonTelega, isp)
    FROM_JSON(jsonTelega, podp_num)
    FROM_JSON(jsonTelega, date)
    FROM_JSON(jsonTelega, date_podp)
    FROM_JSON(jsonTelega, dir)
    FROM_JSON(jsonTelega, tel_num)
    FROM_JSON(jsonTelega, rel)
    FROM_JSON(jsonTelega, kr)
    FROM_JSON(jsonTelega, pril_name)
    FROM_JSON(jsonTelega, pril_count)
    FROM_JSON(jsonTelega, blank)
    FROM_JSON(jsonTelega, gde_sht)
    FROM_JSON(jsonTelega, last_mesto)
}


