//
// Created by user on 04.02.2023.
//
#include <unordered_map>
#include <vector>
#include <string>
#include <filesystem>
#include <iomanip>
#include <iostream>

#ifdef _WIN32
// Windows networking (REQUIRED before windows.h)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "ConverterJSON.h"
#include "SearchServer/SearchServer.h"
#include "scheduler/BackupTask.h"
#include "Commands/GetAttachments/PrefixMap.h"
#include "Commands/GetJsonTelega/Telega.h"

#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"


#define TO_JSON(J, S) J[#S] = val.S;
#define FROM_JSON(J, S) J.at(#S).get_to(val.S);

void ConverterJSON::setSettings(const search_server::Settings &val, const std::string& jsonPath) {
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
    jsonSettings["config"]["year"] = val.year;
    jsonSettings["config"]["hide_mode"] = val.hideMode;
    jsonSettings["Files"] = val.files;
    jsonSettings["config"]["exclude_dirs"] = val.excludeDirs;
    jsonSettings["config"]["prm_base_dir"] = val.prm_base_dir;
    jsonSettings["config"]["prd_base_dir"] = val.prd_base_dir;
    jsonSettings["config"]["compact_threshold_percent"] = val.compactThresholdPercent;

    std::ofstream jsonFileSettings(jsonPath);
    jsonFileSettings << std::setw(2) << jsonSettings;
    jsonFileSettings.close();

}

search_server::Settings ConverterJSON::getSettings(const std::string& jsonPath) {
    /**
    Функция получения настроек сервера из json файла с автодополнением недостающих полей. */

    search_server::Settings s;  // Инициализация с дефолтными значениями
    nh::json jsonSettings;
    std::ifstream jsonFileSettings(jsonPath);
    
    std::vector<std::string> addedFields;
    std::vector<std::string> criticalErrors;
    bool needsResave = false;

    // Проверяем существование файла
    if (!jsonFileSettings.is_open()) {
        LogFile::getStartup().write("WARNING: Settings file not found: " + jsonPath);
        LogFile::getStartup().write("Using default settings. File will be created with defaults.");
        // Все поля будут добавлены как недостающие
        needsResave = true;
        // Создаем пустой JSON для дальнейшей обработки
        jsonSettings = nh::json::object();
        jsonSettings["config"] = nh::json::object();
        jsonSettings["Files"] = nh::json::array();
    } else {
        try
        {
            jsonSettings = nh::json::parse(jsonFileSettings);
            jsonFileSettings.close();
        }
        catch (nh::json::parse_error& ex)
        {
            jsonFileSettings.close();
            std::string errorMsg = "JSON parse error at byte " + std::to_string(ex.byte);
            LogFile::getStartup().write("ERROR: " + errorMsg);
            std::cerr << errorMsg << std::endl;
            throw myExp(jsonPath);
        }
    }

    try
    {

        // Проверяем наличие секции config (создаем если отсутствует)
        if (!jsonSettings.contains("config")) {
            jsonSettings["config"] = nh::json::object();
            needsResave = true;
        }
        
        auto& config = jsonSettings["config"];

        // === Сначала загружаем hide_mode для обработки ошибок ===
        if (config.contains("hide_mode")) {
            config.at("hide_mode").get_to(s.hideMode);
        }

        // === КРИТИЧЕСКИЕ ПОЛЯ (обязательные) ===
        // name
        if (config.contains("Name")) {
            config.at("Name").get_to(s.name);
        } else {
            criticalErrors.push_back("config.Name");
        }

        // dirs - директории для индексации (не могут быть пустыми)
        if (config.contains("dirs") && config["dirs"].is_array() && !config["dirs"].empty()) {
            config.at("dirs").get_to(s.dirs);
        } else {
            criticalErrors.push_back("config.dirs (must be non-empty array)");
        }

        // extensions - расширения файлов (не могут быть пустыми)
        if (config.contains("extensions") && config["extensions"].is_array() && !config["extensions"].empty()) {
            config.at("extensions").get_to(s.extensions);
        } else {
            criticalErrors.push_back("config.extensions (must be non-empty array)");
        }

        // year - год работы (используется в путях к БД)
        if (config.contains("year") && !config["year"].get<std::string>().empty()) {
            config.at("year").get_to(s.year);
        } else {
            criticalErrors.push_back("config.year (required for database paths)");
        }

        // prm_base_dir - не может быть пустым
        if (config.contains("prm_base_dir") && !config["prm_base_dir"].get<std::string>().empty()) {
            config.at("prm_base_dir").get_to(s.prm_base_dir);
        } else {
            criticalErrors.push_back("config.prm_base_dir (cannot be empty)");
        }

        // prd_base_dir - не может быть пустым
        if (config.contains("prd_base_dir") && !config["prd_base_dir"].get<std::string>().empty()) {
            config.at("prd_base_dir").get_to(s.prd_base_dir);
        } else {
            criticalErrors.push_back("config.prd_base_dir (cannot be empty)");
        }

        // === ОПЦИОНАЛЬНЫЕ ПОЛЯ (с автодополнением) ===
        
        // version
        if (config.contains("Version")) {
            config.at("Version").get_to(s.version);
        } else {
            addedFields.push_back("config.Version");
            needsResave = true;
        }

        // max_response
        if (config.contains("max_response")) {
            config.at("max_response").get_to(s.maxResponse);
        } else {
            addedFields.push_back("config.max_response");
            needsResave = true;
        }

        // thread_count
        if (config.contains("thread_count")) {
            config.at("thread_count").get_to(s.threadCount);
        } else {
            addedFields.push_back("config.thread_count");
            needsResave = true;
        }

        // port / asio_port
        if (config.contains("port")) {
            config.at("port").get_to(s.port);
        } else if (config.contains("asio_port")) {
            config.at("asio_port").get_to(s.port);
        } else {
            addedFields.push_back("config.port");
            needsResave = true;
        }

        // dir
        if (config.contains("dir")) {
            config.at("dir").get_to(s.dir);
        } else {
            addedFields.push_back("config.dir");
            needsResave = true;
        }

        // exact_search
        if (config.contains("exact_search")) {
            config.at("exact_search").get_to(s.exactSearch);
        } else {
            addedFields.push_back("config.exact_search");
            needsResave = true;
        }

        // hide_mode (уже загружено выше, но нужно отметить если отсутствовало)
        if (!config.contains("hide_mode")) {
            addedFields.push_back("config.hide_mode");
            needsResave = true;
        }

        // ind_time
        if (config.contains("ind_time")) {
            config.at("ind_time").get_to(s.indTime);
        } else {
            addedFields.push_back("config.ind_time");
            needsResave = true;
        }

        // text_request
        if (config.contains("text_request")) {
            config.at("text_request").get_to(s.requestText);
        } else {
            addedFields.push_back("config.text_request");
            needsResave = true;
        }

        // exclude_dirs
        if (config.contains("exclude_dirs")) {
            config.at("exclude_dirs").get_to(s.excludeDirs);
        } else {
            addedFields.push_back("config.exclude_dirs");
            needsResave = true;
        }

        // compact_threshold_percent (новое поле)
        if (config.contains("compact_threshold_percent")) {
            config.at("compact_threshold_percent").get_to(s.compactThresholdPercent);
        } else {
            addedFields.push_back("config.compact_threshold_percent");
            needsResave = true;
        }

        // Files
        if (jsonSettings.contains("Files")) {
            jsonSettings.at("Files").get_to(s.files);
        } else {
            addedFields.push_back("Files");
            needsResave = true;
        }

        // Если были добавлены поля - пересохраняем настройки
        if (needsResave) {
            setSettings(s, jsonPath);  // Сохраняем в тот же файл, откуда читали
            LogFile::getStartup().write("Settings auto-updated: added " + std::to_string(addedFields.size()) + 
                                       " missing fields with default values");
            LogFile::getStartup().write("Settings saved to: " + jsonPath);
            for (const auto& field : addedFields) {
                LogFile::getStartup().write("  - Added field: " + field);
            }
        }

        // Если есть критические ошибки - логируем и выводим в консоль
        if (!criticalErrors.empty()) {
            // Получаем абсолютный путь к папке logs
            std::filesystem::path logsPath = std::filesystem::absolute("logs");
            std::string logsPathStr = logsPath.string();
            
            // Формируем сообщение для консоли
            std::cout << "\n";
            std::cout << "========================================\n";
            std::cout << "  CRITICAL SETTINGS ERRORS DETECTED!\n";
            std::cout << "========================================\n";
            std::cout << "\nThe following required settings are missing or empty:\n\n";
            for (const auto& err : criticalErrors) {
                std::cout << "  - " << err << "\n";
            }
            std::cout << "\n";
            std::cout << "Server will continue running but may not function correctly.\n";
            std::cout << "Please fix Settings.json and restart the server.\n";
            std::cout << "\n";
            std::cout << "Detailed error log: " << logsPathStr << "/startup.log\n";
            std::cout << "========================================\n";
            std::cout << "\n";
            std::cout.flush();
            
            // Формируем сообщение для MessageBox
            std::string errorMsg = "CRITICAL SETTINGS ERRORS:\n\n";
            for (const auto& err : criticalErrors) {
                errorMsg += "  - Missing or empty: " + err + "\n";
            }
            errorMsg += "\nCheck logs/startup.log for details.";
            
            // Логируем в файл
            LogFile::getStartup().write("=== CRITICAL SETTINGS ERRORS ===");
            for (const auto& err : criticalErrors) {
                LogFile::getStartup().write("CRITICAL: Missing or empty field: " + err);
            }
            LogFile::getStartup().write("Server will continue running but may not function correctly.");
            LogFile::getStartup().write("Please fix Settings.json and restart the server.");
            LogFile::getStartup().write("Logs directory: " + logsPathStr);
            
            // Если hideMode - показываем окно с ошибкой
            if (s.hideMode) {
                ShowWindow(GetConsoleWindow(), SW_SHOW);  // Показываем консоль
                MessageBoxA(nullptr, errorMsg.c_str(), "Search Engine - Critical Settings Error", 
                            MB_OK | MB_ICONERROR | MB_TOPMOST);
            }
            
            // Не выбрасываем исключение - продолжаем работу с ошибками в логе
        }

    }
    catch (const myExp&)
    {
        throw;  // Пробрасываем дальше
    }
    catch (...)
    {
        LogFile::getStartup().write("ERROR: Unknown exception while loading settings");
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


