//
// Created by Sg on 27.10.2024.
//

#include "DocPaths.h"

std::tuple<setLastWriteTimeFiles, setLastWriteTimeFiles> DocPaths::getUpdate(const std::list<std::wstring> &vecDoc) {

    /** Функция возвращающая количество индексируемых файлов*/
    setLastWriteTimeFiles newFiles,del,ind;

    auto myCopy = [](const setLastWriteTimeFiles& first,
                     const setLastWriteTimeFiles& second, setLastWriteTimeFiles& result)
    {
        copy_if(first.begin(),first.end(),std::inserter(result, result.end()),[&second]
                (const auto& item){return !second.contains(item);});
    };

    for(const auto& path:vecDoc)
    {
        size_t pathHash = std::hash<std::wstring>()(path);
        try {
            newFiles.insert(make_pair(pathHash, std::filesystem::last_write_time(path)));
            mapHashDocPaths[pathHash] = path;
        }
        catch(...)
        {
            continue;
        }
    }

    std::thread tInd([&]() { myCopy(newFiles, docPaths2, ind); });
    std::thread tDel([&]() { myCopy(docPaths2, newFiles, del); });
    tInd.join();
    tDel.join();

    for(const auto i:del)
        if(mapHashDocPaths.contains(i.first))
            ind.insert(i);

    docPaths2 = std::move(newFiles);

    return std::make_tuple(ind, del);
}

std::wstring DocPaths::at(size_t _hashFile) const {
    /** Функция возвращающая полный путь к индексируемую файлу по его хэшу*/
    return mapHashDocPaths.at(_hashFile);
}

size_t DocPaths::size() const {
    /** Функция возвращающая количество индексируемых файлов*/
    return mapHashDocPaths.size();
}