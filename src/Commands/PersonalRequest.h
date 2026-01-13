//
// Created by Sg on 02.11.2024.
//

#ifndef SEARCHENGINE_PERSONALREQUEST_H
#define SEARCHENGINE_PERSONALREQUEST_H
#include <iostream>
#include <vector>
#include <boost/serialization/access.hpp>
#include "nlohmann/json.hpp"

namespace boost::archive {
    class portable_binary_oarchive;
    class portable_binary_iarchive;
}



class PersonalRequest {
public:

    // Поля данных
    std::string user_name;
    std::string request;
    std::string request_type;

    // Метод для бинарной сериализации в std::vector<uint8_t>
    [[nodiscard]] std::vector<uint8_t> serializeToBytes() const;

    // Метод для десериализации из std::vector<uint8_t>
    static PersonalRequest deserializeFromBytes(const std::vector<uint8_t>& bytes);

private:

    // Метод для Boost, используемый при сериализации и десериализации
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int /*version*/) {
        ar & user_name;
        ar & request;
        ar & request_type;
    }
};


#endif //SEARCHENGINE_PERSONALREQUEST_H
