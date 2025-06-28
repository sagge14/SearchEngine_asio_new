//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_SAVEFILECMD_H
#define SEARCHENGINE_TEST_SAVEFILECMD_H
#include <utility>
#include <filesystem>
#include "Commands/Command.h"

class SaveFileCmd : public Command {
    // Статическое поле для пути по умолчанию
protected:
    std::wstring defaultSaveDirectory;
public:
    SaveFileCmd() = default;
    explicit SaveFileCmd(std::wstring  _defaultSaveDirectory): defaultSaveDirectory{std::move(_defaultSaveDirectory)}{};

    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) final;

    static std::wstring getUniqueFilename(const std::filesystem::path &directory, const std::wstring &filename,
                                   const std::vector<uint8_t> &fileContent);

    virtual std::filesystem::path getBasePath() = 0;
    static std::wstring getCurrentDate();

    static std::wstring getCurrentMonthInRussianUpperCase();
};


#endif //SEARCHENGINE_TEST_SAVEFILECMD_H
