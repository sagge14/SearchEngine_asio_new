//
// Created by Sg on 11.10.2024.
//

#ifndef SEARCHENGINE_SAVETLGTOSEND_H
#define SEARCHENGINE_SAVETLGTOSEND_H
#include "SaveFileCmd.h"

class SaveTlgToSendCmd : public SaveFileCmd {
    std::filesystem::path getBasePath() override;
public:
    explicit SaveTlgToSendCmd(std::wstring  _defaultSaveDirectory): SaveFileCmd{std::move(_defaultSaveDirectory)}{};
    explicit SaveTlgToSendCmd(std::string  _defaultSaveDirectory);
};


#endif //SEARCHENGINE_SAVETLGTOSEND_H
