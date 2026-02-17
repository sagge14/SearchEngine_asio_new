// record_processor.cpp
#include "RecordProcessor.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "MyUtils/Encoding.h"
#include "MyUtils/LogFile.h"
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <Utils/utf8cpp/utf8.h>

void logKrDebug(const std::string& msg) {
    LogFile::getRecord().write(msg);
}

void cleanupKr(std::string& kr)
{
    replace_all(kr, "\n", " ");
    replace_all(kr, "\r", " ");
    while (kr.find("  ") != std::string::npos)
        replace_all(kr, "  ", " ");
    replace_all(kr, "''", "\"");
    replace_all(kr, "'", "\"");        // или "''" для SQL
    replace_all(kr, "\"\"", "\"");
    replace_all(kr, "\xC2\xA0", "");   // неразрывный пробел
    replace_all(kr, ",", ", ");
}


constexpr size_t MAX_UTF8_FILE_SIZE = 5 * 1024 * 1024; // 5 МБ
constexpr size_t MAX_LINES_COUNT = 50000;              // максимум строк
constexpr size_t MAX_LINE_SIZE = 4096;                 // максимум длина строки

std::string RecordProcessor::srcDbVh_;
std::string RecordProcessor::srcDbIsh_;
std::string RecordProcessor::dstDb_;
std::string RecordProcessor::dstTableVh_;
std::string RecordProcessor::dstTableIsh_;

// =============== вспомогательные ===================

void RecordProcessor::updateField()
{


}

RecordProcessor::RecordProcessor(Telega::TYPE type, int num, bool need_update_kr)
        : type_{type}, num_{num}
{
    LogFile::getRecord().write(L"[RecordProcessor] Constructing: type=" + std::to_wstring(static_cast<int>(type_))
        + L", num=" + std::to_wstring(num_)
        + L", need_update_kr=" + (need_update_kr ? L"true" : L"false"));

    auto conn = SQLiteConnectionManager::instance().getConnection(getSrcDB());
    std::string sql_qry = "SELECT * FROM ARCHIVE WHERE `Index` = " + std::to_string(num_);
    conn->execSql(sql_qry);
    if (conn->empty()) {
        LogFile::getRecord().write(L"[RecordProcessor] No record found for num=" + std::to_wstring(num_));
        empty_ = true;
        return;
    }

    auto row = conn->getBack();

    try {
        ddata_   = row.at("DData");
        podp_    = row.at("PodpNo");
        dpod_    = row.at("DataPodp");
        podrazd_ = row.at("Podrazd");
        replace_all(podrazd_, ";",  "");
        lists_   = row.at("Lists");
        gdesht_  = row.at("GdeSHT");
        kr_from_base_ = row.at(getFieldKr());

        std::string filePath;
        try {
            filePath = row.at("DirectTo") + "\\" + row.at("FileName");
        } catch (const std::exception& e) {
            LogFile::getRecord().write(std::string("[RecordProcessor] File path fields missing: ") + e.what());
            filePath = "";
        }

        if (need_update_kr) {
            try {
                kr_from_text_ = getKrSoderj(filePath);
            } catch (const std::exception& e) {
                LogFile::getRecord().write(std::string("[RecordProcessor] getKrSoderj exception: ") + e.what());
                kr_from_text_ = "";
            }
        } else {
            kr_from_text_ = kr_from_base_;
        }
    } catch (const std::exception& e) {
        LogFile::getRecord().write(std::string("[RecordProcessor] Exception during map value extraction: ") + e.what());
        throw;
    }

    // остальной код (ish_val_) — как было
    ish_val_ = 0;
    try {
        std::string ish2 = gdesht_;
        if (ish2.find("Исх") != std::string::npos) {
            replace_all(ish2, " ", "");
            replace_all(ish2, "Исх.", "");
            replace_all(ish2, "№",   "");
            replace_all(ish2, ";");
            if (auto val = to_int(ish2)) ish_val_ = *val;
        }
    } catch (const std::exception& e) {
        LogFile::getRecord().write(std::string("[RecordProcessor] Exception parsing ish_val: ") + e.what());
    }
}
// ----------------------------------------------

bool RecordProcessor::should_skip()
{
    /* 2. подразделение */
    std::string podr = podrazd_;
    replace_all(podr, ";");

    /* 3. логика исключения */
    bool already_seen = type_ == Telega::TYPE::VHOD ? numberSets_.containsVH(num_) : numberSets_.containsISH(num_) ;
    return ignore_.itsIgnore(podr) || already_seen;
}

// ----------------------------------------------
// формируем VALUES (...) для INSERT
std::string RecordProcessor::get_record() const
{
    // VALUES ('num','ddata','ish','podpno','datapodp','lists','kr')
    return std::format("('{}','{}','{}','{}','{}','{}','{}')",
                       num_, ddata_, ish_val_, podp_, dpod_, lists_, kr_from_text_);
}
// ====================================================================
void RecordProcessor::run() const {
    try {
        static const std::string columns =
                "(num,ddata,ish,podpno,datapodp,lists,kr)";
        static const std::string upsert_sql =
                std::string("INSERT INTO {} ") + columns +
                " VALUES {} "
                "ON CONFLICT(num) DO UPDATE SET "
                "ddata    = excluded.ddata ,"
                "ish      = excluded.ish   ,"
                "podpno   = excluded.podpno,"
                "datapodp = excluded.datapodp,"
                "lists    = excluded.lists ,"
                "kr       = excluded.kr;";    // SQLite ≥ 3.24

        auto dstConn = SQLiteConnectionManager::instance().getConnection(dstDb_);
        std::string values;
        try {
            values = get_record();   // VALUES(...)
        } catch (const std::exception& ex) {
            LogFile::getRecord().write(std::string("[run] Error forming record #") + std::to_string(num_) + " : " + ex.what());
            return;
        }

        if (values.empty()) {
            LogFile::getRecord().write(std::string("[run] Empty values for #") + std::to_string(num_));
            return;
        }

        std::string sql_query;
        try {
            sql_query = std::vformat(upsert_sql, std::make_format_args(getDstTable(), values));
        //    std::cerr << "[run] SQL: " << sql_query << std::endl;
        } catch (const std::exception& ex) {
            LogFile::getRecord().write(std::string("[run] SQL format error for #") + std::to_string(num_) + " : " + ex.what());
            return;
        }

        try {
            dstConn->execSql(sql_query);
     //       std::cerr << "[run] Success for #" << num_ << std::endl;
        } catch (const std::exception& ex) {
         //   std::cerr << "[run] SQL exec error for #" << num_ << " : " << ex.what() << '\n';
        }
    } catch (const std::exception& ex) {
        LogFile::getRecord().write(std::string("[run] FATAL error for #") + std::to_string(num_) + " : " + ex.what());
    } catch (...) {
        LogFile::getRecord().write(std::string("[run] UNKNOWN fatal error for #") + std::to_string(num_));
    }
}


std::string extractKRBlock(const std::vector<std::string>& lines)
{
    static const std::regex reStart(R"(^\s{0,3}=.{4,90})",
                                    std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
    static const std::regex reEnd(R"(^.{4,90}=\s*)",
                                  std::regex::ECMAScript | std::regex::icase | std::regex::optimize);

    int start = -1, end = -1;
    std::string buffer;

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        std::string ln = trim(lines[i]);

        while (ln.find("  ") != std::string::npos)
            replace_all(ln, "  ", " ");

        // пропускаем строки, содержащие "ЛИЧНО" (без учёта регистра)
        if (ln.find("ЛИЧНО") != std::string::npos)
            continue;

        if (start < 0 && std::regex_search(ln, reStart)) {
            start = static_cast<int>(i);
        }

        if (start >= 0) {
            if (!buffer.empty()) buffer += ' ';
            buffer += ln;

            if (std::regex_search(ln, reEnd)) {
                end = static_cast<int>(i);
                break;
            }
        }
    }

    if (start < 0 || end < 0)
        return {};

    // обрезаем по последнему '='
    auto last_eq = buffer.rfind('=');
    if (last_eq != std::string::npos)
        buffer = buffer.substr(0, last_eq + 1);
    else
        return {};

    return trim(buffer);
}

std::string RecordProcessor::getKrSoderj(const std::string& filePath)
{
    auto wfilePath = encoding::utf8_to_wstring(filePath);

    /* ---------- 1. проверяем размер файла ---------- */
    auto fileSize = std::filesystem::file_size(wfilePath);
    if (fileSize > MAX_UTF8_FILE_SIZE) {
        LogFile::getRecord().write(std::string("[RecordProcessor] File too large: ") + std::to_string(fileSize) + " bytes");
        return {};
    }

    std::string utf8;
    try {
        utf8 = encoding::read_oem866_file_as_utf8(wfilePath);
    } catch (const std::exception& e) {
        LogFile::getRecord().write(std::string("[RecordProcessor] Error reading file: ") + e.what());
        return {};
    }

    if (utf8.size() > MAX_UTF8_FILE_SIZE) {
        LogFile::getRecord().write("[RecordProcessor] utf8 string too large after conversion");
        return {};
    }

    std::istringstream iss(utf8);
    std::string line;
    int line_count = 0;
    std::vector<std::string> utf8_without_first_3_lines;
    utf8_without_first_3_lines.reserve(100);
    while (std::getline(iss, line)) {
        if (++line_count <= 3) continue; // пропуск первых 3-х строк
        utf8_without_first_3_lines.push_back(line) ;
    }

    auto kr = extractKRBlock(utf8_without_first_3_lines);

    if (kr.empty()) return {};
    replace_all(kr, "=", "");

    try {
        cleanupKr(kr);
    } catch (const std::exception& e) {
        LogFile::getRecord().write(std::string("[RecordProcessor] Error replacing characters: ") + e.what());
    }

    try {
        if (!utf8::is_valid(kr.begin(), kr.end())) {
            LogFile::getRecord().write("[RecordProcessor] Invalid UTF-8 detected before capitalize_utf8");
            return {};
        }

        kr = capitalize_utf8(trim(kr));

    } catch (const std::exception& e) {
        LogFile::getRecord().write(std::string("[RecordProcessor] Error in capitalize_utf8: ") + e.what());
        return {};
    }

    return kr;
}

// Обрезка строки до max_len символов UTF-8
std::string utf8_truncate(const std::string& s, size_t max_len)
{
    auto it = s.begin();
    auto end = s.end();
    size_t count = 0;

    while (it != end && count < max_len)
    {
        utf8::next(it, end);
        ++count;
    }

    return std::string(s.begin(), it);
}

static std::string prepare_kr(const std::string& src)
{
    std::string kr = src;

    // Все нужные замены
    replace_all(kr, "''", "\"");
    replace_all(kr, "'",  "\"");
    replace_all(kr, "\"\"", "\"");

    // Обрезаем по символам, а не по байтам!
    if (utf8::distance(kr.begin(), kr.end()) > 100)
        kr = utf8_truncate(kr, 100);

    // Экранирование для SQL, если нужно
    replace_all(kr, "'", "''");

    return kr;
}


void RecordProcessor::updateKrShtInShtJurnal() {

    // Для SQL сразу получаем обработанную строку с экранированием
    std::string kr_sql = prepare_kr(kr_from_text_);

    if (!kr_sql.empty()) {
        std::string upd =
                "UPDATE ARCHIVE SET " + getFieldKr() + " = '" + kr_sql +
                "' WHERE `Index` = " + std::to_string(num_) + ';';

        auto srcConn = SQLiteConnectionManager::instance().getConnection(getSrcDB());

        try {
            srcConn->execSql(upd);
        } catch (const std::exception &ex) {
            LogFile::getRecord().write(std::string("UPDATE ARCHIVE для №") + std::to_string(num_) + " не выполнен: " + ex.what());
        }
    } else {
        LogFile::getRecord().write(std::string("empty kr ") + std::to_string(num_));
    }
}


void RecordProcessor::setDefaultDirs(const std::string& _srcDbVh, const std::string& _srcDbIsh,
                                const std::string& _dstDb, const std::string& _dstTableVh,
                                const std::string& _dstTableIsh) {

    srcDbVh_ = _srcDbVh;
    srcDbIsh_ = _srcDbIsh;
    dstTableVh_ = _dstTableVh;
    dstTableIsh_ = _dstTableIsh;
    dstDb_ = _dstDb;

}

std::string RecordProcessor::getDstTable() const {
    return  type_ == Telega::TYPE::VHOD ? dstTableVh_ : dstTableIsh_;
}

std::string RecordProcessor::getSrcDB() const {
    return type_ == Telega::TYPE::VHOD ? srcDbVh_ : srcDbIsh_;
}

std::string RecordProcessor::getFieldKr() const {
    return (type_ == Telega::TYPE::ISHOD) ? "FFrom5" : "PrilName1";
}

std::string RecordProcessor::getPodrazd() {

    return podrazd_;
}

bool RecordProcessor::needUpdate() {
    return kr_from_base_ != kr_from_text_;
}



bool RecordProcessor::setAndCheckLists() {
    // Пробуем распарсить текущее значение
    int int_lists = std::atoi(lists_.c_str());
    if (int_lists > 0) {
        lists_ = std::to_string(int_lists); // нормализуем
        return true;
    }

    // Получаем значение из БД, если своё поле невалидно
    lists_ = getListsFieldValue(type_, num_);
    int_lists = std::atoi(lists_.c_str());
    if (int_lists > 0) {
        lists_ = std::to_string(int_lists); // нормализуем
        return true;
    }

    lists_.clear();
    return false;
}

std::string RecordProcessor::getListsFieldValue(Telega::TYPE type, int num) {

    auto srcDb =  type == Telega::TYPE::VHOD ? srcDbVh_ : srcDbIsh_;

    auto srcConn = SQLiteConnectionManager::instance().getConnection(srcDb);
    std::string sql_qry = "SELECT Lists FROM ARCHIVE WHERE `Index` = " + std::to_string(num);
    srcConn->execSql(sql_qry);

    if (srcConn->empty()) {
        return "0";
    }

    // 3. Парсим значение из БД
    auto row = srcConn->getBack();
    auto lists = row.at("Lists"); // Обновляем поле

    return lists;
}



