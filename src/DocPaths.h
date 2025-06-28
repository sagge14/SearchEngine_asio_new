//
// Created by Sg on 27.10.2024.
//

#ifndef SEARCHENGINE_TEST_DOCPATHS_H
#define SEARCHENGINE_TEST_DOCPATHS_H

#include <string>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <tuple>
#include <fstream>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/unordered_set.hpp>

#include <boost/serialization/list.hpp>
#include <boost/serialization/utility.hpp>

#include <filesystem>

#include "SerializationHelpers.h"

class hashFunction {
    /*  std::time_t toTime_t2(filesystem::file_time_type tp) const
     {
          return chrono::system_clock::to_time_t(chrono::file_clock::to_sys(tp));
     } */
    template <typename TP>
    [[nodiscard]] std::time_t toTime_t(TP tp) const
    {
        using namespace std::chrono;
        auto sctp = time_point_cast<system_clock::duration>(tp - TP::clock::now()
                                                            + system_clock::now());
        return system_clock::to_time_t(sctp);
    }

public:

    size_t operator()(const std::pair<size_t, std::filesystem::file_time_type>& p) const
    {
        return p.first ^ std::hash<time_t>()(toTime_t(p.second));
    }

};

typedef std::unordered_set<std::pair<size_t, std::filesystem::file_time_type>, hashFunction> setLastWriteTimeFiles;

class DocPaths {

public:
    std::unordered_map<size_t, std::wstring> mapHashDocPaths;
    setLastWriteTimeFiles docPaths2;

    [[nodiscard]] std::wstring at(size_t hashFile) const;
    [[nodiscard]] size_t size() const;
    std::tuple<setLastWriteTimeFiles, setLastWriteTimeFiles> getUpdate(const std::list<std::wstring>& vecDoc);

    DocPaths() = default;

private:
    // Метод serialize для Boost.Serialization
    friend class boost::serialization::access;

    template<class Archive>
    void serialize(Archive& ar, const unsigned int version)
    {
        ar & mapHashDocPaths;
        ar & docPaths2;
    }
};


#endif //SEARCHENGINE_TEST_DOCPATHS_H
