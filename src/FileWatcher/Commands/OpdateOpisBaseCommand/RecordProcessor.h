// record_processor.h
#pragma once
#include "SQLite/mySql.h"
#include "utils_help.h"
#include "PodrIgnore.h"
#include "NumberSets.h"
#include <unordered_set>
#include <format>
#include "Commands/GetJsonTelega/Telega.h"

class RecordProcessor
{
public:

    RecordProcessor(Telega::TYPE type, int num, bool need_update_kr);

    void run() const;

    void updateKrShtInShtJurnal();

    static void setDefaultDirs(const std::string& srcDbVh,    const std::string& srcDbIsh,
            const std::string& dstDb,    const std::string& dstTableVh,
            const std::string& dstTableIsh);

    std::string getPodrazd();
    bool needUpdate();


    static std::string getKrSoderj(const std::string& filePath);
    bool setAndCheckLists();
    static std::string getListsFieldValue(Telega::TYPE type, int num);
    int getNum() {return num_;};

private:
    static std::string dstDb_, srcDbVh_, srcDbIsh_, dstTableVh_, dstTableIsh_;
    PodrIgnore      ignore_;
    NumberSets numberSets_{};
    Telega::TYPE type_;
    int num_, ish_val_;
    bool empty_;
    std::string ddata_;
    std::string podp_;
    std::string dpod_;
    std::string lists_;
    std::string podrazd_;
    std::string kr_from_base_;
    std::string gdesht_;
    std::string kr_from_text_;

    std::string getDstTable() const;
    std::string getSrcDB() const;
    std::string getFieldKr() const;




    std::string get_record() const;
    bool        should_skip();

    void updateField();
};
