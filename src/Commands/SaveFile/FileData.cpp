#include "FileData.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include "boost/archive/portable_binary_oarchive.hpp"
#include "boost/archive/portable_binary_iarchive.hpp"
// Реализация конструктора с двумя параметрами
FileData::FileData(const std::string& filename, const std::vector<uint8_t>& data)
        : filename(filename), data(data) {}

// Реализация конструктора с filepath
FileData::FileData(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filepath);
    }
    std::filesystem::path filePath = filepath;

    filename = filePath.filename().string();

    data = std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
}

const std::string& FileData::getFilename() const {
    return filename;
}

const std::vector<uint8_t>& FileData::getData() const {
    return data;
}

std::ostream& operator<<(std::ostream& os, const FileData& fd) {
    os << "Filename: " << fd.filename << ", Data size: " << fd.data.size();
    return os;
}

template<class Archive>
void FileData::serialize(Archive& ar, const unsigned int version) {
    ar & filename;
    ar & data;
}

// Специализация шаблона должна быть в том же файле, что и определение класса
template void FileData::serialize<portable_binary_oarchive>(portable_binary_oarchive&, const unsigned int);
template void FileData::serialize<portable_binary_iarchive>(portable_binary_iarchive&, const unsigned int);

// Функция для сериализации объекта в вектор байтов
std::vector<uint8_t> serializeToBytes(const FileData& fileData) {
    try {
        std::ostringstream oss(std::ios::binary);
        portable_binary_oarchive oa(oss);
        oa << fileData;
        const std::string& str = oss.str();
        return std::vector<uint8_t>(str.begin(), str.end());
    } catch (const std::exception& e) {
        std::cerr << "Serialization error: " << e.what() << std::endl;
        throw;
    }
}

// Функция для десериализации объекта из вектора байтов
FileData deserializeFromBytes(const std::vector<uint8_t>& bytes) {
    try {
        std::string str(bytes.begin(), bytes.end());
        std::istringstream iss(str, std::ios::binary);
        portable_binary_iarchive ia(iss);
        FileData fileData;
        ia >> fileData;
        return fileData;
    } catch (const std::exception& e) {
        std::cerr << "Deserialization error: " << e.what() << std::endl;
        throw;
    }
}
