//
// Created by Sg on 23.05.2024.
//

#ifndef SEARCHENGINE_TEST_SOME_FUNC_H
#define SEARCHENGINE_TEST_SOME_FUNC_H
#include <iostream>
#include "SQLite/mySql.h"


class some_func {

};

std::string getDocs(const std::string &request) {
    //getRemoteIP()
    std::string ip = "1.1.1.1";
    mySql::excSql("SELECT ID FROM IP_ID WHERE IP ='" + ip  + "'");

    if(SQL_EMPTY)
        return "";

    nh::json jsonResolutions;


    auto id = SQL_FRONT_VALUE(ID);
    mySql::excSql("SELECT DISTINCT N_SHT FROM EXECUTORS WHERE ID = " + id);


    std::vector<std::string> vDocs;
    std::map<std::string, std::string> mExcs;

    for(auto& r:SQL_INST)
        vDocs.push_back(r.at("N_SHT"));

    jsonResolutions["docs"] = vDocs;

    mySql::excSql("SELECT ID_EX, NNAME FROM R_EX WHERE ID_R = " + id);

    for(auto& r:SQL_INST)
        mExcs.insert(std::make_pair(r.at("NNAME"),r.at("ID_EX")));

    jsonResolutions["execs"] = mExcs;

    return to_string(jsonResolutions);
}
std::string asio_server::Interface::getDoc(const std::string &_request)  {

    // getRemoteIP();
    std::string ip = "1.1.1.1";
    mySql::excSql("SELECT ID FROM IP_ID WHERE IP ='" + ip + "'");

    if(SQL_EMPTY)
        return "";

    std::vector<std::string> vCom,vRes;

    auto id = SQL_FRONT_VALUE(ID);

    mySql::excSql("SELECT N_RES FROM EXECUTORS WHERE ID = " + id + " AND N_SHT = " + _request);

    if(!SQL_EMPTY)
        for(auto& r:SQL_INST)
            vRes.push_back(r.at("N_RES"));

    mySql::excSql("SELECT N_COM FROM COMMENTS WHERE ID = " + id + " AND N_SHT = " + _request);

    if(!SQL_EMPTY)
        for(auto& r:SQL_INST)
            vCom.push_back(r.at("N_COM"));

    nh::json jsonDoc;

    mySql::excSql("SELECT SHT_DIR FROM RESOLUTIONS WHERE N_SHT = " + _request);

    jsonDoc["sht_dir"] = SQL_FRONT_VALUE(SHT_DIR);
    jsonDoc["res"] = vRes;
    jsonDoc["com"] = vCom;

    return to_string(jsonDoc);
}

std::string asio_server::Interface::getResolution(const std::string& _request) {

    mySql::excSql("SELECT * FROM RESOLUTIONS WHERE N_RES = " + _request);

    if(SQL_EMPTY)
        return "";

    nh::json jsonResolution;
    auto request = SQL_FRONT_VALUE(TEXT_DIR);
    jsonResolution["text_res"] = getFile("");

    return to_string(jsonResolution);
}

std::string asio_server::Interface::addResolution(const std::string& _request) {

    //getRemoteIP()
    std::string ip = "1.1.1.1";

    mySql::excSql("SELECT ID FROM IP_ID WHERE IP ='" + ip + "'");

    if(SQL_EMPTY)
        return "";

    nh::json jsonResolutions;

    auto id = SQL_FRONT_VALUE(ID);

    Resolution r = ConverterJSON::getResolution(_request);
    r.id_r = id;
    r.chk = "0";
    r.addResolution();
    r.saveResolutions();

    return "ok";
}

#endif //SEARCHENGINE_TEST_SOME_FUNC_H
