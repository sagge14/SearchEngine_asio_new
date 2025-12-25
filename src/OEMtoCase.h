#pragma once
#include <map>
#include <fstream>
#include <string>
#include <cctype>
#include <filesystem>




class OEMtoCase
{

    std::string getExecutableDir();

public:
    OEMtoCase() = default;
    // Конструктор — грузит файл соответствий
    // Первая строка файла — строчные, вторая — прописные



    explicit OEMtoCase(const std::string& path)
    {
        auto full_path = getExecutableDir() + "\\" + path;
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Файл не найден или не является обычным файлом: " + path);
        }

        std::ifstream file(full_path, std::ios::binary);
        if (!file.is_open())
            throw std::runtime_error("Can't open OEM table: " + path);

        auto trim_crlf = [](std::string& s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        };

        std::string lower, upper;
        std::getline(file, lower);
        trim_crlf(lower);
        std::getline(file, upper);
        trim_crlf(upper);


        if (lower.size() != upper.size())
            throw std::runtime_error("OEM table format error: lines length mismatch");

        for (size_t i = 0; i < lower.size(); ++i) {
            upperMap_[lower[i]] = upper[i]; // строчная → прописная
            lowerMap_[upper[i]] = lower[i]; // прописная → строчная
        }
    }

    // Получить ВЕРХНИЙ регистр символа (по таблице)
    static char to_upper(char c)
    {
        if (auto it = inst_->upperMap_.find(c); it != inst_->upperMap_.end())
            return it->second;
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    // Получить НИЖНИЙ регистр символа (по таблице)
   static char to_lower(char c)
    {
        if (auto it = inst_->lowerMap_.find(c); it != inst_->lowerMap_.end())
            return it->second;
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Перевести строку в верхний регистр
    static std::string to_upper(const std::string& s)
    {
        std::string out = s;
        for (auto& ch : out)
            ch = OEMtoCase::to_upper(ch);
        return out;
    }

    // Перевести строку в нижний регистр
    static std::string to_lower(const std::string& s)
    {
        std::string out = s;
        for (auto& ch : out)
            ch = OEMtoCase::to_lower(ch);
        return out;
    }
    static OEMtoCase& getInst()
    {
        return *inst_;
    }
    static void init(const std::string& path)
    {
        if(!inst_)
            inst_ = new OEMtoCase(path);
    }

private:
    std::map<char, char> upperMap_; // строчная → прописная
    std::map<char, char> lowerMap_; // прописная → строчная
    static inline OEMtoCase* inst_ = nullptr;
};