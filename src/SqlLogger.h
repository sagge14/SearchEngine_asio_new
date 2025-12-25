#pragma once

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "SQLite/mySQLite.h"
#include "Commands/SearchServer/SearchServerCmd.h"
#include "Commands/PersonalRequest.h"
#include "nlohmann/json.hpp"

namespace nh = nlohmann;

class SqlLogger {


public:
    // Получение экземпляра Singleton
    static SqlLogger& instance(const std::string& dbPath = "");

    // Асинхронное добавление логов
    void logRequest(const PersonalRequest& request);

    void logJson(const nh::json& jsonSettings);

    // Удалить копирование
    SqlLogger(const SqlLogger&) = delete;
    SqlLogger& operator=(const SqlLogger&) = delete;

private:
    // Приватный конструктор и деструктор
    explicit SqlLogger(const std::string& dbPath);
    ~SqlLogger();

    void initializeDatabase();
    void processQueue();
    static std::pair<std::string, std::string> getCurrentDateTime();

private:
    mySQLite db;

    std::queue<PersonalRequest> logQueue;
    std::thread workerThread;
    std::mutex queueMutex;
    std::condition_variable queueCondVar;
    std::atomic<bool> stopWorker{false};
};

namespace logutil {
    inline void log(const PersonalRequest& pr) {
        try {
            SqlLogger::instance().logRequest(pr);
        } catch (...) {
            // Безопасно игнорируем ошибку логгирования
        }
    }
// Удобная inline-функция для логирования
    inline void log(std::string user, std::string request, std::string type) {
        try {
            SqlLogger::instance().logRequest(PersonalRequest{user, request, type});
        } catch (...) {
            // Безопасно игнорируем ошибку логгирования
        }
    }
    inline void safe_log(std::string user, std::string request, std::string type) {
        try {
            SqlLogger::instance().logRequest(PersonalRequest{std::move(user), std::move(request), std::move(type)});
        } catch (const std::exception& e) {
            std::cout << "[LOG FAIL] " << e.what() << std::endl;
        } catch (...) {
            std::cout << "[LOG FAIL] unknown exception" << std::endl;
        }
    }
    inline void safe_log(PersonalRequest pr) {
        try {
            SqlLogger::instance().logRequest(pr);
        } catch (const std::exception& e) {
            std::cout << "[LOG FAIL] " << e.what() << std::endl;
        } catch (...) {
            std::cout << "[LOG FAIL] unknown exception" << std::endl;
        }
    }

}