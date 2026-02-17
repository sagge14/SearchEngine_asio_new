#pragma once
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <boost/serialization/access.hpp>
#include "SerializationHelpers.h"

struct UpdatePack {
    std::vector<uint32_t> added;     // новые файлы
    std::vector<uint32_t> updated;   // изменилось mtime/size
    std::vector<uint32_t> removed;   // исчезли
};

class DocPaths
{
public:
    /* === API для InvertedIndex ===================================== */
    UpdatePack getUpdate(const std::vector<std::wstring>& paths);   // полный рескан
    [[nodiscard]] const std::wstring& pathById(uint32_t id) const;                // id  -> путь
    [[nodiscard]] bool needUpdate(uint32_t id,
                    std::filesystem::file_time_type newTime,
                    uint64_t newSize) const;                       // точечная проверка

    void markRemoved(uint32_t id);                                  // удалить по id

    std::pair<uint32_t,bool>         // {id, changed}
    upsert(const std::wstring& path,
           std::filesystem::file_time_type mtime,
           uint64_t                     fsize);

    bool tryGetId(const std::wstring& path, uint32_t& outId) const;

    /* === сериализация ============================================= */
    template<class Ar> void serialize(Ar& ar, const unsigned) {
        ar & id2path & path2info;
    }

    size_t size() const {return path2info.size();}

private:
    struct FileInfo {
        std::filesystem::file_time_type mtime;
        uint64_t fsize{};
        uint32_t id{};
        template<class Ar> void serialize(Ar& ar, const unsigned) {
            ar & mtime & fsize & id;
        }
    };

    // ключ – полный путь к файлу
    std::unordered_map<std::wstring, FileInfo> path2info;
    // обратное преобразование id -> путь  (индекс == id)
    std::vector<std::wstring>                  id2path;

    uint32_t nextId() { return static_cast<uint32_t>(id2path.size()); }
};
