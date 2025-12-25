#include "DocPaths.h"
#include <fstream>   // для отладочного лога (по желанию)

UpdatePack DocPaths::getUpdate(const std::vector<std::wstring>& paths)
{
    UpdatePack pack;
    std::unordered_set<std::wstring> seen;

    /* ── 1. проход по сканированным файлам ───────────────────────── */
    for (const auto& p : paths)
    {
        seen.insert(p);

        auto mtime = std::filesystem::last_write_time(p);
        auto fsize = std::filesystem::file_size(p);

        auto it = path2info.find(p);
        if (it == path2info.end())
        {   /* новый файл */
            uint32_t id = nextId();
            id2path.push_back(p);
            path2info.emplace(p, FileInfo{mtime, fsize, id});
            pack.added.push_back(id);
        }
        else
        {   /* файл уже был */
            seen.insert(p);
            auto& info = it->second;
            if (info.mtime != mtime || info.fsize != fsize)
            {
                info.mtime = mtime;
                info.fsize = fsize;
                pack.updated.push_back(info.id);
            }
        }
    }

    /* ── 2. ищем исчезнувшие файлы ───────────────────────────────── */
    for (auto it = path2info.begin(); it != path2info.end(); )
    {
        if (!seen.contains(it->first))
        {
            pack.removed.push_back(it->second.id);
            id2path[it->second.id].clear();        // tombstone
            it = path2info.erase(it);
        }
        else
            ++it;
    }
    return pack;
}

/* --- сервисные методы ------------------------------------------- */

const std::wstring& DocPaths::pathById(uint32_t id) const
{
    return id2path[id];       // O(1)
}

bool DocPaths::needUpdate(uint32_t id,
                          std::filesystem::file_time_type newTime,
                          uint64_t newSize) const
{
    if (id >= id2path.size() || id2path[id].empty()) return true;   // нет такого id
    const auto& info = path2info.at(id2path[id]);
    return info.mtime != newTime || info.fsize != newSize;
}

void DocPaths::markRemoved(uint32_t id)
{
    if (id >= id2path.size() || id2path[id].empty()) return;
    path2info.erase(id2path[id]);
    id2path[id].clear();
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
        id2path.push_back(path);
        path2info.emplace(path, FileInfo{mtime, fsize, id});
        return {id, true};
    }



    FileInfo& info = it->second;
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
