//
// Created by Sg on 12.10.2024.
//

#include "md5Hasher.h"
#include <boost/uuid/detail/md5.hpp>
#include <boost/algorithm/hex.hpp>
#include <fstream>

std::string md5Hasher::calculateMD5(const std::wstring& fileName) {
    std::ifstream file(fileName, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file");
    }

    boost::uuids::detail::md5 md5;
    std::vector<char> buffer(1024 * 1024);
    boost::uuids::detail::md5::digest_type digest;

    while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
        md5.process_bytes(buffer.data(), file.gcount());
    }

    md5.get_digest(digest);

    const auto charDigest = reinterpret_cast<const char*>(&digest);
    std::string result;
    boost::algorithm::hex(charDigest, charDigest + sizeof(digest), std::back_inserter(result));
    return result;
}