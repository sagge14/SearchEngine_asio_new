#include <iostream>
#include <thread>
#include <vector>

#include "SearchServer.h"
#include "ConverterJSON.h"
#include "Commands/GetJsonTelega/Telega.h"
#include "AsioServer.h"
#include "Commands/GetTelegaWay/TelegaWay.h"

#include <filesystem>

#include "SqlLogger.h"

#include <exception>
#include <fstream>

void myTerminateHandler() {
    std::ofstream log("fatal_crash.log", std::ios::app);
    log << "[FATAL] Server terminated due to unhandled exception!" << std::endl;
    std::abort(); // обязательно, иначе программа "зависнет"
}

#include <windows.h>
#include <dbghelp.h>

LONG WINAPI myUnhandledFilter(EXCEPTION_POINTERS* ExceptionInfo) {
    std::ofstream log("crash.log", std::ios::app);
    log << "[SEH] Exception code: 0x" << std::hex << ExceptionInfo->ExceptionRecord->ExceptionCode << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}



#include <codecvt>
namespace cc3  {
    // Преобразуем std::wstring в UTF-8 std::string
    std::string wstringToUtf8(const std::wstring &wstr) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.to_bytes(wstr);
    }

    // Преобразуем UTF-8 std::string в std::wstring
    std::wstring utf8ToWstring(const std::string &str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        return conv.from_bytes(str);
    }
}
void writeBytesToFile(const std::vector<uint8_t>& bytes, const std::filesystem::path& filePath) {
    std::ofstream outFile(filePath, std::ios::binary);
    if (!outFile) {
        throw std::runtime_error("Failed to open file for writing: " + filePath.string());
    }
    outFile.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    outFile.close();
}
int main() {

    using namespace std;
/**/
    std::set_terminate(myTerminateHandler);
    SetUnhandledExceptionFilter(myUnhandledFilter);

    auto settings = ConverterJSON::getSettings();
    TelegaWay::base_way_dir = "D:\\F12\\" + settings.year + ".db";
    TelegaWay::base_f12_dir = "D:\\F12\\base.db";
    TelegaWay::work_year = settings.year;
    std::locale::global(std::locale("Russian_Russia.866"));

    SqlLogger::instance("log.db"); // инициализация

    if (settings.hideMode) {
        HWND hWnd = GetConsoleWindow();
        ShowWindow(hWnd, SW_HIDE);
    }

    Telega::year = settings.year;
    Telega::prd_base_dir = settings.prd_base_dir;
    Telega::prm_base_dir = settings.prm_base_dir;
    Telega::b_prm = Telega::getBases(Telega::TYPE::VHOD);
    Telega::b_prm = Telega::getBases(Telega::TYPE::ISHOD);

    try {
        asio_server::Interface::setYear(settings.year);
        search_server::SearchServer myServer(settings);
        boost::asio::io_context io_context;
        asio_server::Interface::setSearchServer(&myServer);

        auto asioServer = std::make_shared<asio_server::AsioServer>(io_context, settings.port);

        // Создание пула потоков
        const size_t thread_pool_size = std::thread::hardware_concurrency();
        std::vector<std::thread> thread_pool;
        for (size_t i = 0; i < thread_pool_size; ++i) {
            thread_pool.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        string command;

        while (true) {
            cout << "---Search Engine is started!---" << endl;
            cout << "----Search Engine work now!----" << endl;
            cout << "------------- Menu ------------" << endl;
            cout << "'1' for exit." << endl;
            cout << "-------------------------------" << endl;
            cout << "Enter the command: ";

            cin >> command;
            cin.clear();

            if (command == "1") {
                cout << "Stopping server..." << endl;
                io_context.stop();
                break;
            } else {
                cout << "Unknown command" << endl;
            }

            // Очистка экрана
            // system("cls");
        }

        // Ожидание завершения всех потоков
        for (auto& thread : thread_pool) {
            if (thread.joinable()) {
                thread.join();
            }
        }

    } catch (ConverterJSON::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    } catch (search_server::SearchServer::myExp& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
    }

    // Очистка экрана
    // system("cls");
    std::cout << "---Bye, bye!---" << endl;
    system("pause");

    return 0;
}
