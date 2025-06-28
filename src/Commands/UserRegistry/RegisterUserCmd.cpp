#include "RegisterUserCmd.h"
#include <cstring>

std::vector<uint8_t> RegisterUserCmd::execute(const std::vector<uint8_t>& username_bytes) {

    std::string username = std::string(username_bytes.begin(), username_bytes.end());
    size_t user_id;
    std::vector<uint8_t> byteVector(sizeof(size_t));
    try
    {
        user_id = UserRegistry::getInstance().registerUser(username);
    }
    catch (std::exception& e)
    {
        auto se = e.what();
        std::memcpy(byteVector.data(), &se, sizeof(se));
        return byteVector;
    }

    std::memcpy(byteVector.data(), &user_id, sizeof(size_t));

    return byteVector;
}
