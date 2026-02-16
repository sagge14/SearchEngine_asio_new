#include "SqlLogger.h"
#include "Encoding.h"
#include <iostream>
#include <sstream>
#include <string>
#include <regex>
#include <algorithm>
#include <cctype>

inline bool isLikelyFilePath(const std::string& str) {
    if (str.size() < 3) return false;

    if (std::isalpha(str[0]) && str[1] == ':' && (str[2] == '\\' || str[2] == '/'))
        return true;

    std::regex ext(R"(\.(txt|docx|log|shp|json|xml|ini|pdf|db))", std::regex::icase);
    if (std::regex_search(str, ext)) return true;

    return false;
}




SqlLogger::SqlLogger(const std::string& dbPath) : db(dbPath) {
    initializeDatabase();
    // Запуск потока для обработки очереди
    workerThread = std::thread(&SqlLogger::processQueue, this);
}

SqlLogger::~SqlLogger() {
    // Останавливаем поток и ждем завершения
    stopWorker = true;
    queueCondVar.notify_all();
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

// Инициализация базы данных
void SqlLogger::initializeDatabase() {
    std::string createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            date DATE,
            time TIME,
            request_type VARCHAR,
            request_text VARCHAR,
            user_name VARCHAR
        );
    )";
    db.execSql(createTableSQL);
}



std::string summarizeSql(const std::string& sql) {
    if (sql.rfind("where", 0) == 0)
        return sql;

    std::string field, value, limit;
    std::smatch match;

    if (std::regex_search(sql, match, std::regex(R"(upper\s*\(\s*`?(\w+)`?\s*\))", std::regex_constants::icase)))
        field = match[1].str();

    if (std::regex_search(sql, match, std::regex(R"(like\s*\(\s*'%([^%']+)%'\s*\))", std::regex_constants::icase)))
        value = match[1].str();

    if (std::regex_search(sql, match, std::regex(R"(limit\s+(\d+))", std::regex_constants::icase)))
        limit = match[1].str();

    std::ostringstream out;
    if (!field.empty()) out << "field: " << field << " ";
    if (!value.empty()) out << "value: " << value << " ";
    if (!limit.empty()) out << "limit: " << limit;

    std::string result = out.str();
    return result.empty() ? sql : result;  // <= важно
}




// Асинхронное добавление логов
void SqlLogger::logRequest(const PersonalRequest& request) {
    // Добавляем лог в очередь и уведомляем поток
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        logQueue.push(request);
    }
    queueCondVar.notify_one();
}

// Функция получения текущих даты и времени
std::pair<std::string, std::string> SqlLogger::getCurrentDateTime() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream dateStream, timeStream;
    dateStream << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d");
    timeStream << std::put_time(std::localtime(&in_time_t), "%H:%M:%S");
    return {dateStream.str(), timeStream.str()};
}

// Метод для обработки очереди
void SqlLogger::processQueue() {

    while (!stopWorker) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCondVar.wait(lock, [this] { return !logQueue.empty() || stopWorker; });

        while (!logQueue.empty()) {
            PersonalRequest request = logQueue.front();
            logQueue.pop();
            lock.unlock();

            std::cout << "- new " << summarizeSql(request.request) << std::endl;

            auto [date, time] = getCurrentDateTime();

            // Экранируем одинарные кавычки для SQL
            auto escapeQuotes = [](const std::string& input) -> std::string {
                std::string result;
                for (char c : input) {
                    result += c;
                    if (c == '\'') result += '\'';  // SQL escape
                }
                return result;
            };

            if (isLikelyFilePath(request.request)) {
                request.request = encoding::win1251_to_utf8(request.request);
            } else if (request.request_type == "GET_SQL_JSON_ANSWER") {
                request.request = encoding::oem866_to_utf8(request.request);
            }

            request.request = summarizeSql(request.request);

            std::string escapedRequestText = escapeQuotes(request.request);
            std::string escapedUser = escapeQuotes(request.user_name);
            std::string escapedType = escapeQuotes(request.request_type);

            std::string insertSQL =
                    "INSERT INTO logs (date, time, request_type, request_text, user_name) "
                    "VALUES ('" + date + "', '" + time + "', '" +
                    escapedType + "', '" + escapedRequestText + "', '" + escapedUser + "');";

            try {
                db.execSql(insertSQL);
            } catch (const std::exception& e) {
                std::cerr << "Log DB error: " << e.what() << "\nQuery: " << insertSQL << std::endl;
            }

            lock.lock();  // Блокируем снова для доступа к очереди
        }
    }
}


// Singleton-геттер
SqlLogger& SqlLogger::instance(const std::string& dbPath) {
    static SqlLogger* instance_ = nullptr;
    static std::mutex init_mutex;

    std::lock_guard<std::mutex> lock(init_mutex);
    if (!instance_) {
        if (dbPath.empty()) {
            throw std::runtime_error("SqlLogger must be initialized with database path before first use.");
        }
        instance_ = new SqlLogger(dbPath);
    }
    return *instance_;
}

void SqlLogger::logJson(const nh::json& json_log) {
    auto table_name = json_log.value("table_name", "");

    if(table_name.empty())
    {
        //log error in file
        return;
    }

    auto fields = json_log.value("fields", nh::json::array());
    if(fields.empty())
    {
        //log error in file
        return;
    }

}



