//
// Created by user on 09.09.2023.
//
#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <map>
#include <filesystem>
#include <list>
#include <vector>

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
    bool deleted{false};   // файл-источник удалён с диска (след в индексе сохранён)

    static inline std::vector<std::string> b_prm = {};
    static inline std::vector<std::string> b_prd = {};
    static inline std::string prm_base_dir = {};
    static inline std::string prd_base_dir = {};
    static inline std::string year = {};

    enum class SourceAvailability
    {
        Disabled,
        Configured,
        Unavailable
    };

    struct SourceError : std::runtime_error
    {
        SourceAvailability availability;
        TYPE sourceType;

        SourceError(
            SourceAvailability availability,
            TYPE sourceType,
            const std::string& message)
            : std::runtime_error(message)
            , availability(availability)
            , sourceType(sourceType)
        {
        }
    };

    void initTelega(const std::map<std::string, std::string>& _record);
    Telega(TYPE _t, const std::filesystem::path& _p, float _rel = 1, bool _deleted = false)
        :type{_t},dir{_p.string()}, rel{_rel}, deleted{_deleted}{};

public:

    enum class TYPE : int
    {
        VHOD,
        ISHOD,
        NOTTG
    };


    [[maybe_unused]] static std::vector<std::string> getBases (const std::string& _dir);
    static std::list<std::map<std::string,std::string>> findBase (const std::string& condition, TYPE _type, bool single = false);
    static std::vector<std::string> getBases(TYPE _type);
    static Telega::TYPE getTypeFromDir(const std::filesystem::path& p);

    static std::string getNumFromFileName(const std::filesystem::path& path);

    [[nodiscard]] static const std::string& baseDir(TYPE type) noexcept;
    [[nodiscard]] static bool isSourceConfigured(TYPE type) noexcept;
    [[nodiscard]] static const char* sourceLabel(TYPE type) noexcept;
    [[nodiscard]] static std::string disabledDiagnostic(TYPE type);
    [[nodiscard]] static std::string unavailableDiagnostic(TYPE type);
    [[nodiscard]] static SourceAvailability probeSource(TYPE type);
    /// Lazy-load b_prm / b_prd for one source. Disabled → SourceError(Disabled).
    /// Missing/inaccessible configured directory → SourceError(Unavailable).
    static void ensureBasesLoaded(TYPE type);
    [[nodiscard]] static std::string archiveDbPathFor(TYPE type);

    Telega(const std::map<std::string, std::string>& _record, TYPE _type, float _rel = 1);
    explicit Telega(const std::filesystem::path& p, float _rel = 1, bool _deleted = false);

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
