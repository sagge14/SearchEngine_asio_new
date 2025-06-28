#ifndef USERREGISTRY_H
#define USERREGISTRY_H

#include <unordered_map>
#include <string>
#include <mutex>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/access.hpp>

class UserRegistry {
private:
    std::unordered_map<std::string, size_t> username_to_id;
    std::unordered_map<size_t, std::string> id_to_username;
    size_t next_id;
    std::mutex registry_mutex;
    const std::string FILENAME = "users.dat";

    // Private constructor for Singleton pattern
    UserRegistry();
    ~UserRegistry();

    // Serialization support
    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive & ar, const unsigned int version) {
        ar & username_to_id;
        ar & id_to_username;
        ar & next_id;
    }

public:
    // Meyers' Singleton
    static UserRegistry& getInstance();

    // Delete copy constructor and assignment operator
    UserRegistry(const UserRegistry&) = delete;
    UserRegistry& operator=(const UserRegistry&) = delete;

    bool isValidUsername(const std::string& username);
    size_t registerUser(const std::string& username);
    void saveToFile();
    void loadFromFile();
};

#endif // USERREGISTRY_H
