#include "DocPaths.h"
#include <fstream>   // для отладочного лога (по желанию)

DocPaths::DocPaths(const DocPaths& other)
    : path2info(other.path2info)
{
    rebuildId2Path(other.id2path.size());
}

DocPaths& DocPaths::operator=(const DocPaths& other)
{
    if (this != &other)
    {
        path2info = other.path2info;
        rebuildId2Path(other.id2path.size());
    }
    return *this;
}

DocPaths::DocPaths(DocPaths&& other)
    : path2info(std::move(other.path2info))
{
    rebuildId2Path(other.id2path.size());
    other.rebuildId2Path();
}

DocPaths& DocPaths::operator=(DocPaths&& other)
{
    if (this != &other)
    {
        const size_t idCount = other.id2path.size();
        path2info = std::move(other.path2info);
        rebuildId2Path(idCount);
        other.rebuildId2Path();
    }
    return *this;
}

void DocPaths::rebuildId2Path(size_t minimumSize)
{
    size_t requiredSize = minimumSize;
    for (const auto& [path, info] : path2info)
    {
        const size_t infoSize = static_cast<size_t>(info.id) + 1;
        if (infoSize > requiredSize)
            requiredSize = infoSize;
    }

    id2path.assign(requiredSize, nullptr);
    for (const auto& [path, info] : path2info)
        id2path[info.id] = &path;
}

void DocPaths::rebuildId2Path(
    const std::vector<std::wstring>& serializedId2Path)
{
    id2path.assign(serializedId2Path.size(), nullptr);
    for (size_t id = 0; id < serializedId2Path.size(); ++id)
    {
        if (serializedId2Path[id].empty())
            continue;
        auto it = path2info.find(serializedId2Path[id]);
        if (it != path2info.end())
            id2path[id] = &it->first;
    }
}

std::vector<DocPaths::RawRow> DocPaths::exportRows() const
{
    std::vector<RawRow> out;
    out.reserve(path2info.size());
    for (const auto& [path, info] : path2info)
    {
        RawRow r;
        r.id = info.id;
        r.path = path;
        r.fsize = info.fsize;
        r.mtimeTicks = info.mtime.time_since_epoch().count();
        r.deleted = info.deleted;
        out.push_back(std::move(r));
    }
    return out;
}

void DocPaths::rebuildFromRows(std::vector<RawRow>&& rows)
{
    path2info.clear();
    id2path.clear();

    uint32_t maxId = 0;
    for (const auto& r : rows)
        if (r.id > maxId) maxId = r.id;

    id2path.resize(static_cast<size_t>(maxId) + 1);

    for (auto& r : rows)
    {
        if (r.id >= id2path.size())
            id2path.resize(static_cast<size_t>(r.id) + 1);

        std::filesystem::file_time_type::duration dur{ r.mtimeTicks };
        std::filesystem::file_time_type mtime{ dur };
        auto it = path2info.emplace(
            std::move(r.path),
            FileInfo{ mtime, r.fsize, r.id, r.deleted }).first;
        id2path[r.id] = &it->first;
    }
}

UpdatePack DocPaths::getUpdate(const std::vector<std::wstring>& paths)
{
    UpdatePack pack;
    std::unordered_set<std::wstring_view> seen;
    seen.reserve(paths.size());

    /* ── 1. проход по сканированным файлам ───────────────────────── */
    for (const auto& p : paths)
    {
        seen.emplace(p);

        auto mtime = std::filesystem::last_write_time(p);
        auto fsize = std::filesystem::file_size(p);

        auto it = path2info.find(p);
        if (it == path2info.end())
        {   /* новый файл */
            uint32_t id = nextId();
            auto inserted = path2info.emplace(
                p, FileInfo{mtime, fsize, id, false}).first;
            id2path.push_back(&inserted->first);
            pack.added.push_back(id);
        }
        else
        {   /* файл уже был */
            auto& info = it->second;
            if (info.deleted)
            {   /* файл вернулся на диск -> реактивация + переиндексация */
                info.deleted = false;
                info.mtime = mtime;
                info.fsize = fsize;
                pack.updated.push_back(info.id);
            }
            else if (info.mtime != mtime || info.fsize != fsize)
            {
                info.mtime = mtime;
                info.fsize = fsize;
                pack.updated.push_back(info.id);
            }
        }
    }

    /* ── 2. ищем исчезнувшие файлы ───────────────────────────────── */
    /* Вечный след: не удаляем запись, а только помечаем deleted.       */
    for (auto& [path, info] : path2info)
    {
        if (!info.deleted && !seen.contains(std::wstring_view(path)))
        {
            info.deleted = true;
            pack.removed.push_back(info.id);
        }
    }
    return pack;
}

/* --- сервисные методы ------------------------------------------- */

const std::wstring& DocPaths::pathById(uint32_t id) const
{
    static const std::wstring emptyPath;
    const auto* path = id2path[id];
    return path != nullptr ? *path : emptyPath;       // O(1)
}

bool DocPaths::needUpdate(uint32_t id,
                          std::filesystem::file_time_type newTime,
                          uint64_t newSize) const
{
    if (id >= id2path.size() || id2path[id] == nullptr) return true;   // нет такого id
    const auto& info = path2info.at(*id2path[id]);
    return info.mtime != newTime || info.fsize != newSize;
}

void DocPaths::markRemoved(uint32_t id)
{
    if (id >= id2path.size() || id2path[id] == nullptr) return;
    auto it = path2info.find(*id2path[id]);
    if (it != path2info.end())
        it->second.deleted = true;   // вечный след: путь и метаданные сохраняем
}

bool DocPaths::isDeleted(uint32_t id) const
{
    if (id >= id2path.size() || id2path[id] == nullptr) return false;
    auto it = path2info.find(*id2path[id]);
    return it != path2info.end() && it->second.deleted;
}

bool DocPaths::getInfo(uint32_t id, int64_t& mtimeTicks, uint64_t& fsize) const
{
    if (id >= id2path.size() || id2path[id] == nullptr) return false;
    auto it = path2info.find(*id2path[id]);
    if (it == path2info.end()) return false;
    mtimeTicks = it->second.mtime.time_since_epoch().count();
    fsize = it->second.fsize;
    return true;
}


std::pair<uint32_t,bool>
DocPaths::upsert(const std::wstring& path,
                 std::filesystem::file_time_type mtime,
                 uint64_t fsize)
{
    auto it = path2info.find(path);
    if (it == path2info.end())            // --- новый файл ---
    {
        uint32_t id = nextId();
        auto inserted = path2info.emplace(
            path, FileInfo{mtime, fsize, id}).first;
        id2path.push_back(&inserted->first);
        return {id, true};
    }



    FileInfo& info = it->second;
    if (info.deleted)                                 // --- вернулся после удаления ---
    {
        info.deleted = false;
        info.mtime = mtime;
        info.fsize = fsize;
        return {info.id, true};
    }
    if (info.mtime != mtime || info.fsize != fsize)   // --- изменился ---
    {
        info.mtime = mtime;
        info.fsize = fsize;
        return {info.id, true};
    }
    return {info.id, false};              // --- всё актуально ---
}

bool DocPaths::tryGetId(const std::wstring& path, uint32_t& outId) const
{
    auto it = path2info.find(path);
    if (it == path2info.end()) return false;
    outId = it->second.id;
    return true;
}
