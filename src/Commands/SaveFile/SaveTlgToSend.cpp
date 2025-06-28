//
// Created by Sg on 11.10.2024.
//

#include "SaveTlgToSend.h"

std::filesystem::path SaveTlgToSendCmd::getBasePath() {

    std::wstring currentDateFolder = getCurrentDate();
    std::wstring currentMonthFolder = getCurrentMonthInRussianUpperCase();

    return std::filesystem::path(SaveFileCmd::defaultSaveDirectory) / currentMonthFolder / currentDateFolder;

}

SaveTlgToSendCmd::SaveTlgToSendCmd(std::string _defaultSaveDirectory) {

}


