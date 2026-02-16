//
// Created by Sg on 11.10.2024.
//

#include "SaveDefaultCmd.h"
#include "MyUtils/Encoding.h"


std::filesystem::path SaveFileDefaultCmd::getBasePath() {

    return std::filesystem::path{SaveFileCmd::defaultSaveDirectory};

}

SaveFileDefaultCmd::SaveFileDefaultCmd(const std::string& _defaultSaveDirectory) {
    defaultSaveDirectory = std::filesystem::path{encoding::utf8_to_wstring(_defaultSaveDirectory)};
}