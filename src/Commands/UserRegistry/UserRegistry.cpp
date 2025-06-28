#include "UserRegistry.h"
#include <regex>
#include <stdexcept>
#include <fstream>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

UserRegistry::UserRegistry() : next_id(1) {
    // Load data from file if it exists
    std::ifstream infile(FILENAME, std::ios::binary);
    if (infile.good()) {
        loadFromFile();
    }
}

UserRegistry::~UserRegistry() {
    saveToFile();
}

UserRegistry& UserRegistry::getInstance() {
    static UserRegistry instance;
    return instance;
}

bool UserRegistry::isValidUsername(const std::string& username) {
    // Check if the username meets the criteria
    std::regex username_regex("^[a-zA-Z_][a-zA-Z0-9_]{2,}$");
    return std::regex_match(username, username_regex);
}

size_t UserRegistry::registerUser(const std::string& username) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (!isValidUsername(username)) {
        throw std::invalid_argument("Invalid username");
    }

    if (username_to_id.find(username) != username_to_id.end()) {
        throw std::invalid_argument("Username already exists");
    }

    size_t user_id = next_id++;
    username_to_id[username] = user_id;
    id_to_username[user_id] = username;

    return user_id;
}

void UserRegistry::saveToFile() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::ofstream out(FILENAME, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Unable to open file for writing");
    }
    boost::archive::binary_oarchive oa(out);
    oa << *this;
    out.close();
}

void UserRegistry::loadFromFile() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    std::ifstream in(FILENAME, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Unable to open file for reading");
    }
    boost::archive::binary_iarchive ia(in);
    ia >> *this;
    in.close();
}
