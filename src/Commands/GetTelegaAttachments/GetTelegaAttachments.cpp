//
// Created by Sg on 31.08.2025.
//
#include "nlohmann/json.hpp"
#include "SQLite/mySql.h"
#include <string>
#include "GetTelegaAttachments.h"

#include <codecvt>
#include <locale>

inline std::string to_utf8(const std::wstring &wstr) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(wstr);
}

inline std::wstring from_utf8(const std::string &str) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str);
}

std::vector<uint8_t> GetTelegaAttachmentsCmd::execute(const std::vector<uint8_t> &_data) {
    namespace nh = nlohmann;
    namespace fs = std::filesystem;

    std::string str_request(_data.begin(), _data.end());
    nh::json jsonRequest;
    try {
        jsonRequest = nh::json::parse(str_request);
    } catch (...) {
        return {};
    }


    int tlg_id = jsonRequest.value("id", -1);
    if (tlg_id < 0) return {};

    int int_type = jsonRequest.value("type", -1);
    auto tlg_type = static_cast<Telega::TYPE>(int_type);

    const decltype(Telega::getBases(Telega::TYPE{})) *base;
    switch (tlg_type) {
        case Telega::TYPE::VHOD:  base = &Telega::b_prm; break; //int_type == 0
        case Telega::TYPE::ISHOD: base = &Telega::b_prd; break; //int_type == 1
        default: return {};
    }

    std::string sql_qry =
            "SELECT PrilName, DirectTo FROM archive WHERE `index` = '" + std::to_string(tlg_id) + "'";

    std::string attachments_names;
    std::string attachments_dir;

    for (const auto &base_name: *base) {
        SQL_INST.excSql(base_name, sql_qry);
        if (!SQL_EMPTY) {
            attachments_names = SQL_FRONT_VALUE(PrilName);
            attachments_dir   = SQL_FRONT_VALUE(DirectTo);
            break;
        }
    }

    nh::json jres;
    jres = nh::json::array();

    auto getItem = [&attachments_dir] (const std::string& name)
    {
        fs::path full = fs::path(attachments_dir) / name;
        nh::json item;
        auto full2 = fs::path{from_utf8(full.string())}; // имя файла в UTF-8
        item["name"]   = full.filename().string();
        item["exists"] = fs::exists(full2);
        item["size"]   = item["exists"] ? fs::file_size(full2) : 0;

        return  item;
    };

    if(attachments_names.empty())
    {
        jres = std::vector<uint8_t>{};
        std::string out = jres.dump();
        return {out.begin(), out.end()};
    }

    if(tlg_type == Telega::TYPE::VHOD)
    {
        // разделяем имена по ";"
        std::stringstream ss(attachments_names);
        std::string name;
        while (std::getline(ss, name, ';')) {
            if (name.empty()) continue;

            jres.push_back(getItem(name));
        }
    }
    else {
        auto name = std::to_string(tlg_id) + ".zip";
        jres.push_back(getItem(name));
    }

    std::string out = jres.dump();
    return {out.begin(), out.end()};
}

