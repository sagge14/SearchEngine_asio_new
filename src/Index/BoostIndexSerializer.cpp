#include "BoostIndexSerializer.h"

#include "InvertedIndex.h"

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace inverted_index {

BoostIndexSerializer::BoostIndexSerializer(std::string path)
    : path_(std::move(path))
{
}

std::string BoostIndexSerializer::kind() const { return "boost-binary"; }

bool BoostIndexSerializer::exists() const
{
    return !path_.empty() && std::filesystem::exists(path_);
}

void BoostIndexSerializer::save(const InvertedIndex& idx)
{
    if (path_.empty())
        throw std::runtime_error("BoostIndexSerializer: empty path");

    std::ofstream ofs(path_, std::ios::binary);
    if (!ofs.is_open())
        throw std::runtime_error("BoostIndexSerializer: failed to open file for write: " + path_);

    boost::archive::binary_oarchive oa(ofs);
    oa << idx;
}

void BoostIndexSerializer::load(InvertedIndex& idx)
{
    if (path_.empty())
        throw std::runtime_error("BoostIndexSerializer: empty path");

    std::ifstream ifs(path_, std::ios::binary);
    if (!ifs.is_open())
        throw std::runtime_error("BoostIndexSerializer: failed to open file for read: " + path_);

    boost::archive::binary_iarchive ia(ifs);
    ia >> idx;
}

} // namespace inverted_index

