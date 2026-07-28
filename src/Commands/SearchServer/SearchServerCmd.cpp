//
// Created by Sg on 23.05.2024.
//

#include "SearchServerCmd.h"
#include <boost/archive/portable_binary_oarchive.hpp>
#include <boost/archive/portable_binary_iarchive.hpp>
#include <boost/serialization/string.hpp>
#include "Commands/PersonalRequest.h"
#include <sstream>


std::vector<uint8_t> SoloRequestCmd::getAnswerBytes(const std::string& _request)
{


    // Получение ответа от сервера
    auto results = server_->getAnswer(_request);

    // Запись результатов в строку.
    // Формат additive: к прежним "path;relevance" добавлена третья колонка
    // deleted (1 = файл удалён с диска, след сохранён; 0 = присутствует).
    std::string result_str;
    for (const auto& result : results) {
        result_str += result.path + ";" + std::to_string(result.relevance) + ";" +
                      (result.deleted ? "1" : "0") + "\n";
    }

    // Преобразование строки в вектор байт
    std::vector<uint8_t> result_bytes(result_str.begin(), result_str.end());

    return result_bytes;
}
std::vector<uint8_t> SoloRequestCmd::execute(const std::vector<uint8_t>& data)
{
    std::string request(data.begin(), data.end());
    return getAnswerBytes(request);
}
std::vector<uint8_t> StartDictionaryUpdateCmd::execute(const std::vector<uint8_t>& data)
{
    try
    {
        server_->flushUpdateAndSaveDictionary();
        return {1};
    }
    catch(...)
    {
        return {0};
    }

}

std::vector<uint8_t> PersonalSoloRequestCmd::execute(const std::vector<uint8_t> &data) {

    auto pr = PersonalRequest::deserializeFromBytes(data);
    return getAnswerBytes(pr.request);
}


