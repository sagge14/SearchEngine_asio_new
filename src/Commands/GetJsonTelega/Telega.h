//
// Created by user on 09.09.2023.
//
#pragma once
#include <cstddef>
#include <string>
#include <map>
#include <filesystem>
#include <list>

struct Telega {

    enum class TYPE;
    std::string num;
    TYPE type;
    std::string from_to;
    std::string kr;
    std::string isp;
    std::string podp_num;
    std::string date;
    std::string date_podp;
    std::string dir;
    std::string tel_num;
    std::string pril_name;
    std::string pril_count;
    std::string blank;
    std::string last_mesto;
    std::string gde_sht;
    float rel;

    static inline std::vector<std::string> b_prm = {};
    static inline std::vector<std::string> b_prd = {};
    static inline std::string prm_base_dir = {};
    static inline std::string prd_base_dir = {};
    static inline std::string year = {};

    void initTelega(const std::map<std::string, std::string>& _record);
    Telega(TYPE _t, const std::filesystem::path& _p, float _rel = 1):type{_t},dir{_p.string()}, rel{_rel}{};

public:

    enum class TYPE : int
    {
        VHOD,
        ISHOD,
        NOTTG
    };


    static std::vector<std::string> getBases (const std::string& _dir);
    static std::list<std::map<std::string,std::string>> findBase (const std::string& condition, TYPE _type, bool single = false);
    static std::vector<std::string> getBases(TYPE _type);
    static Telega::TYPE getTypeFromDir(const std::filesystem::path& p);

    static std::string getNumFromFileName(const std::filesystem::path& path);

    Telega(const std::map<std::string, std::string>& _record, TYPE _type, float _rel = 1);
    explicit Telega(const std::filesystem::path& p, float _rel = 1);

    Telega& operator==(Telega&& t)
    {
        #define MOVE(m) this->m = std::move(t.m);
        MOVE(num)
        MOVE(type);
        MOVE(from_to);
        MOVE(isp);
        MOVE(podp_num);
        MOVE(date);
        MOVE(date_podp);
        MOVE(dir);
        MOVE(tel_num);
        MOVE(rel);
        MOVE(pril_name);
        MOVE(pril_count);
        MOVE(blank);
        MOVE(last_mesto);
        MOVE(gde_sht);
        return *this;
    }
    bool operator==(const Telega &other) const
    {
        return (num == other.num && type == other.type);
    }

    bool operator<(const Telega &r) const
    {
        return (num < r.num && type == r.type);
    }
};
