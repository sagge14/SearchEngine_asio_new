//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_SEARCHSERVERCMD_H
#define SEARCHENGINE_TEST_SEARCHSERVERCMD_H
#include "Commands/Command.h"
#include "SearchServer/SearchServer.h"

class SearchServerCmd : public Command {
protected:
    search_server::SearchServer* server_;
public:
    explicit SearchServerCmd(search_server::SearchServer* server) : server_{server}{};
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data)  override = 0;
};

class SoloRequestCmd : public SearchServerCmd {
protected:
    std::vector<uint8_t> getAnswerBytes(const std::string& request);
public:
    explicit SoloRequestCmd(search_server::SearchServer* server) : SearchServerCmd{server}{};
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) override;


};

class PersonalSoloRequestCmd : public SoloRequestCmd {

public:
    explicit PersonalSoloRequestCmd(search_server::SearchServer* server) : SoloRequestCmd{server}{};

    std::vector<uint8_t> execute(const std::vector<uint8_t>& data) final;

};

class StartDictionaryUpdateCmd : public SearchServerCmd {
public:
    explicit StartDictionaryUpdateCmd(search_server::SearchServer* server) : SearchServerCmd{server}{};
    std::vector<uint8_t> execute(const std::vector<uint8_t>& data = {}) override;
};

//lass

#endif //SEARCHENGINE_TEST_SEARCHSERVERCMD_H
