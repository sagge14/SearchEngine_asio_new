//
// Created by Sg on 11.10.2024.
//

#ifndef SEARCHENGINE_SAVERAZN_H
#define SEARCHENGINE_SAVERAZN_H
#include "SaveFileCmd.h"

class SaveFileDefaultCmd : public SaveFileCmd {
    std::filesystem::path getBasePath() override;
public:
    explicit SaveFileDefaultCmd(std::wstring  _defaultSaveDirectory): SaveFileCmd{std::move(_defaultSaveDirectory)}{};
    explicit SaveFileDefaultCmd(const std::string& _defaultSaveDirectory);
};


#endif //SEARCHENGINE_SAVERAZN_H
