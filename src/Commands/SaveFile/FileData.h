#ifndef FILEDATA_H
#define FILEDATA_H

#include <iostream>
#include <string>
#include <vector>

#include <boost/serialization/vector.hpp>
#include <boost/serialization/string.hpp>
//#include <boost/archive/portable_iarchive.hpp>
//#include <boost/archive/portable_oarchive.hpp>
#include <codecvt>
#include <locale>
#include <cstdint>

// Класс FileData для хранения имени файла и его данных

class FileData {

public:

    FileData() = default;
    FileData(const std::string& filename, const std::vector<uint8_t>& data);
    explicit FileData(const std::string& filepath);  // Новый конструктор

    [[nodiscard]] const std::string& getFilename() const;
    [[nodiscard]] const std::vector<uint8_t>& getData() const;

    friend std::ostream& operator<<(std::ostream& os, const FileData& fd);

private:

    std::string filename;
    std::vector<uint8_t> data;

    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive& ar, unsigned int version);
};


std::vector<uint8_t> serializeToBytes(const FileData& fileData);
FileData deserializeFromBytes(const std::vector<uint8_t>& bytes);


#endif // FILEDATA_H
