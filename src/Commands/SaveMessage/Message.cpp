
#include "Message.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <stdexcept>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>

namespace boost::serialization {

        template<class Archive>
        void serialize(Archive& ar, std::filesystem::path& p, const unsigned int version) {
            std::string str = p.string(); // Сериализуем путь как строку
            ar & str;
            if (Archive::is_loading::value) {
                p = std::filesystem::path(str); // Преобразуем строку обратно в путь при десериализации
            }
        }

    }

void Message::addAttachment(const std::filesystem::path& _file_dir)
{
    std::ifstream file(_file_dir, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open file: " + _file_dir.string());

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    attachments[_file_dir.filename().string()] = data;
}

void Message::saveToDirectory(const std::filesystem::path& dirPath) const
{
    std::filesystem::create_directory(dirPath);

    for (const auto& [relative_path, data] : attachments) {
        // Construct the full path including any subdirectories
        //std::wstring relative_path = myConverter::utf8ToWstring(relative_path_str);
        std::filesystem::path full_path = dirPath / std::filesystem::path(relative_path);

        // Создаем все необходимые директории для вложенных путей
        std::filesystem::create_directories(full_path.parent_path());

        std::ofstream file(full_path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Failed to open attachment file for writing: " + relative_path.string());

        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
    }

}

Message Message::loadFromDirectory(const std::filesystem::path& dirPath)
{
    Message msg;

    // Recursive directory traversal for attachments
    for (auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
        if (std::filesystem::is_regular_file(entry)) {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) {
                throw std::runtime_error("Failed to open attachment file: " + entry.path().string());
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
            
            // Save with relative path
            std::filesystem::path relative_path = std::filesystem::relative(entry.path(), dirPath);
            msg.attachments[relative_path.string()] = data;
        }
    }

    return msg;
}

std::vector<uint8_t> Message::serializeToBytes(const Message &fileData) {
    std::ostringstream oss(std::ios::binary);
    portable_binary_oarchive oa(oss);
    oa << fileData;
    const std::string& str = oss.str();

    std::vector<BYTE> buffer;

    // Перемещаем символы из строки в вектор с использованием индекса
    buffer.reserve(str.size());  // Резервируем память заранее для повышения эффективности
    for (char i : str) {
        buffer.push_back(static_cast<BYTE>(i));  // Добавляем символ в вектор
    }

    return buffer;
}

Message Message::deserializeFromBytes(const std::vector<uint8_t> &bytes) {
    // Используем array_source для чтения данных непосредственно из вектора
    std::string str(bytes.begin(), bytes.end());
    std::istringstream iss(str, std::ios::binary);

    portable_binary_iarchive ia(iss);  // Используем portable_binary архив для десериализации

    Message message;
    ia >> message;  // Десериализуем объект Message

    return message;
}
