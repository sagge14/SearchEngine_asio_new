//
// Created by Sg on 02.11.2024.
//

#include "PersonalRequest.h"
#include <boost/archive/portable_binary_oarchive.hpp>
#include <boost/archive/portable_binary_iarchive.hpp>
#include <boost/serialization/string.hpp>
#include <sstream>

std::vector<uint8_t> PersonalRequest::serializeToBytes() const {
    std::ostringstream oss(std::ios::binary);
    portable_binary_oarchive oa(oss);
    oa << *this;
    std::string data = oss.str();
    return std::vector<uint8_t>(data.begin(), data.end());
}

PersonalRequest PersonalRequest::deserializeFromBytes(const std::vector<uint8_t>& bytes) {
    std::istringstream iss(std::string(bytes.begin(), bytes.end()), std::ios::binary);
    portable_binary_iarchive ia(iss);
    PersonalRequest req;
    ia >> req;
    return req;
}

