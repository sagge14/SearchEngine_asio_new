//
// Created by Sg on 29.10.2024.
//

#include "GetAttachmentsCmd.h"
#include "Commands/SaveFile/FileData.h"
#include "ConverterJSON.h"
#include "PrefixMap.h"
void GetAttachmentsCmd::deleteDirectory(const std::filesystem::path& dirPath) {
    try {
        if (std::filesystem::exists(dirPath)) {
            std::filesystem::remove_all(dirPath);
            std::cout << "Директория удалена успешно: " << dirPath << std::endl;
        } else {
            std::cout << "Директория не существует: " << dirPath << std::endl;
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Ошибка при удалении директории: " << e.what() << std::endl;
    }
}
std::vector<uint8_t> GetAttachmentsCmd::execute(const std::vector<uint8_t> &data) {

    //std::filesystem::path path = "example.json";
    PrefixMap pm = ConverterJSON::loadAttachPrefixLogin("prefix_map.json");

    auto user_name = std::wstring(data.begin(), data.end());
    std::filesystem::path path;
    std::string message = "Operator no found!";

    try {
        path = pm.getPath(user_name);
    }catch(...)
    {
        return std::vector<uint8_t>(message.begin(),message.end());
    }

     if(!std::filesystem::exists(path)) {
        message = "Net prilojeniy!";
        return std::vector<uint8_t>(message.begin(),message.end());
    }

    Message m = Message::loadFromDirectory(path);

    deleteDirectory(path);

    return Message::serializeToBytes(m);
}
