#pragma once
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <boost/serialization/access.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include "SerializationHelpers.h"

struct UpdatePack {
    std::vector<uint32_t> added;     // новые файлы
    std::vector<uint32_t> updated;   // изменилось mtime/size
    std::vector<uint32_t> removed;   // исчезли
};

class DocPaths
{
public:
    DocPaths() = default;
    DocPaths(const DocPaths& other);
    DocPaths& operator=(const DocPaths& other);
    DocPaths(DocPaths&& other);
    DocPaths& operator=(DocPaths&& other);

    /* === API для InvertedIndex ===================================== */
    UpdatePack getUpdate(const std::vector<std::wstring>& paths);   // полный рескан
    [[nodiscard]] const std::wstring& pathById(uint32_t id) const;                // id  -> путь
    [[nodiscard]] bool needUpdate(uint32_t id,
                    std::filesystem::file_time_type newTime,
                    uint64_t newSize) const;                       // точечная проверка

    void markRemoved(uint32_t id);                                  // пометить как удалённый (след сохраняется)

    /// Файл помечен как удалённый (исчез с диска), но след в индексе сохранён.
    [[nodiscard]] bool isDeleted(uint32_t id) const;

    /// Получить метаданные файла по id (для live-зеркала). false, если нет.
    [[nodiscard]] bool getInfo(uint32_t id, int64_t& mtimeTicks, uint64_t& fsize) const;

    std::pair<uint32_t,bool>         // {id, changed}
    upsert(const std::wstring& path,
           std::filesystem::file_time_type mtime,
           uint64_t                     fsize);

    bool tryGetId(const std::wstring& path, uint32_t& outId) const;

    struct RawRow {
        uint32_t id{};
        std::wstring path;
        int64_t mtimeTicks{};   // ticks of std::filesystem::file_time_type::duration
        uint64_t fsize{};
        bool deleted{false};
    };

    /// Экспорт состояния для внешнего хранилища (SQLite/и т.д.).
    [[nodiscard]] std::vector<RawRow> exportRows() const;

    /// Восстановить состояние из внешнего хранилища.
    /// Полностью перезаписывает текущее содержимое.
    void rebuildFromRows(std::vector<RawRow>&& rows);

    /* === сериализация ============================================= */
    template<class Ar> void save(Ar& ar, const unsigned) const {
        std::vector<std::wstring> serializedId2Path;
        serializedId2Path.reserve(id2path.size());
        for (const auto* path : id2path)
        {
            if (path != nullptr)
                serializedId2Path.push_back(*path);
            else
                serializedId2Path.emplace_back();
        }
        ar & serializedId2Path & path2info;
    }

    template<class Ar> void load(Ar& ar, const unsigned) {
        std::vector<std::wstring> serializedId2Path;
        ar & serializedId2Path & path2info;
        rebuildId2Path(serializedId2Path);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    size_t size() const {return path2info.size();}

    /** Сжать внутренние контейнеры. Вызывать в моменты,
        когда нет одновременных upsert/markRemoved. */
    void shrinkToFit() {
        id2path.shrink_to_fit();
        path2info.rehash(0);
    }

private:
    struct FileInfo {
        std::filesystem::file_time_type mtime;
        uint64_t fsize{};
        uint32_t id{};
        bool deleted{false};   // файл удалён с диска, но след сохранён
        template<class Ar> void serialize(Ar& ar, const unsigned) {
            ar & mtime & fsize & id & deleted;
        }
    };

    // ключ – полный путь к файлу
    std::unordered_map<std::wstring, FileInfo> path2info;
    // обратное преобразование id -> ключ path2info  (индекс == id)
    std::vector<const std::wstring*>           id2path;

    uint32_t nextId() { return static_cast<uint32_t>(id2path.size()); }
    void rebuildId2Path(size_t minimumSize = 0);
    void rebuildId2Path(const std::vector<std::wstring>& serializedId2Path);
};
