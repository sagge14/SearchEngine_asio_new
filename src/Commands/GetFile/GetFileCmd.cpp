//
// Created by Sg on 23.05.2024.
//

#include "GetFileCmd.h"
#include "fstream"
std::vector<uint8_t> GetFileCmd::execute(const std::vector<uint8_t>& data) {
    return action(data);
}
std::vector<uint8_t> GetFileCmd::downloadFileByPath(const std::string& data) {
    return GetFileCmd::downloadFileByPath(std::vector<uint8_t>{data.begin(), data.end()});
}
std::vector<uint8_t> GetFileCmd::downloadFileByPath(const std::vector<uint8_t> &data) {
    // Преобразуем вектор байт в строку, чтобы использовать его как имя файла
    std::string filename(data.begin(), data.end());

    // Открываем файл в бинарном режиме
    std::fstream file(filename, std::ios::in | std::ios::binary);
    if (file.is_open()) {
        // Читаем содержимое файла в вектор байт
        std::vector<uint8_t> file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return file_content;
    }

    // Возвращаем пустой вектор, если файл не удалось открыть
    return {};
}


