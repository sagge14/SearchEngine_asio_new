#include "AsioServer.h"
#include "SearchServer.h"
#include "SQLite/mySql.h"
#include "SqlLogger.h"
#include <iostream>
#include <memory>
#include <utility>
#include <filesystem>
#include "Commands/SearchServer/SearchServerCmd.h"
#include "Commands/SaveFile/SaveTlgToSend.h"
#include "Commands/SaveFile/SaveDefaultCmd.h"
#include "Commands/GetFile/GetFileCmd.h"
#include "Commands/SaveMessage/SaveMessageCmd.h"
#include "Commands/SaveMessage/GetMessageCmd.h"
#include "Commands/GetJsonTelega/GetJsonTelegaCmd.h"
#include "Commands/UserRegistry/RegisterUserCmd.h"
#include "Commands/ServiceCommands/PingCmd.h"
#include "Commands/GetTelegaWay/GetTelegaWayCmd.h"
#include "Commands/GetAttachments/GetAttachmentsCmd.h"

#include <mutex>


using boost::asio::ip::tcp;

void asio_server::session::start() {
    readHeader();
}

asio_server::session::~session()
{
    try {
        boost::system::error_code ec;
        if (socket_.is_open()) {
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }
        logutil::log(userName_,"EMPTY","DISCONNECT");
    } catch (...) {
        // безопасно игнорируем любые исключения
    }
}

void asio_server::session::readHeader() {
    auto self(shared_from_this());
    try {
        if (socket_.is_open()) {
            std::cout << "Connect\t\t" + getRemoteIP() << std::endl;
            search_server::SearchServer::addToLog("Connect\t\t" + getRemoteIP());

            socket_.async_read_some(boost::asio::buffer(&header_, sizeof(header_)),
                                    [this, self](boost::system::error_code ec, std::size_t length) {
                                        try {
                                            if (!ec && trustCommand()) {
                                                std::cout << "Header read\t" + getRemoteIP() << std::endl;
                                                search_server::SearchServer::addToLog("Header read\t" + getRemoteIP());
                                                readData();
                                            } else {
                                                header_.command = COMMAND::SOMEERROR;
                                                search_server::SearchServer::addToLog("Received from\t" + getRemoteIP() + "\tcommand\t'" + getTextCommand(header_.command) + "'");
                                                writeHeader();
                                                std::cout << "Socket off - - \t" + getRemoteIP() << std::endl;
                                                search_server::SearchServer::addToLog("Socket off\t" + getRemoteIP());
                                            }
                                        } catch (const std::exception& e) {
                                            search_server::SearchServer::addToLog(std::string("Exception in readHeader: ") + e.what());
                                            socket_.close();
                                        }
                                    });
        } else {
            search_server::SearchServer::addToLog("Socket is not open when trying to read header.");
        }
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in readHeader: ") + e.what());
        socket_.close();
    }
}

void asio_server::session::readData() {
    auto self(shared_from_this());

    try {
        std::size_t to_read = std::min(static_cast<size_t>(header_.size - v_data_.size()), static_cast<size_t>(max_length));
        v_data_.resize(v_data_.size() + to_read);

        socket_.async_read_some(boost::asio::buffer(v_data_.data() + v_data_.size() - to_read, to_read),
                                [this, self, to_read](boost::system::error_code ec, std::size_t length) {
                                    try {
                                        if (!ec) {
                                            if (length > to_read) {
                                                std::cerr << "Error: read more than expected!" << std::endl;
                                                socket_.close();
                                                return;
                                            }
                                            std::lock_guard<std::mutex> lock(v_data_mutex);
                                            v_data_.resize(v_data_.size() - to_read + length);

                                            auto now = std::chrono::system_clock::now();
                                            std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                                            std::tm now_tm = *std::localtime(&now_time);

                                            // Форматирование времени и даты
                                            char buffer[80];
                                            std::strftime(buffer, sizeof(buffer), "%H:%M:%S %d.%m.%Y", &now_tm);

                                            if (v_data_.size() < header_.size) {
                                                // Вывод с датой и временем
                                             //   std::cout << "Received from\t" + getRemoteIP() + "\tcommand '" + getTextCommand(header_.command) << " " << std::to_string(to_read) << " " << v_data_.size() << "\tat " << buffer << std::endl;
                                                readData();
                                            } else {
                                            //    std::cout << "Received from\t" + getRemoteIP() + "\tcommand '" + getTextCommand(header_.command) << " " << std::to_string(to_read) << " " << v_data_.size() << "\tat " << buffer << std::endl;
                                                std::cout << "Received all data. Processing..." << std::endl;
                                                commandExec();
                                            }
                                        } else {
                                            header_.command = COMMAND::SOMEERROR;
                                            std::cerr << "Error on receive: " << ec.message() << std::endl;
                                            socket_.close();
                                        }
                                    } catch (const std::exception& e) {
                                        search_server::SearchServer::addToLog(std::string("Exception in readData: ") + e.what());
                                        socket_.close();
                                    }
                                });
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in readData: ") + e.what());
        socket_.close();
    }
}

/*
template<>
void asio_server::session::writeToSocket(const File<std::vector<BYTE>>& _data) {
    auto self(shared_from_this());

    for(int i = 0; i < _data.getBlocksCount(); i++) {
        auto count = _data[i].size();
        boost::asio::async_write(socket_, boost::asio::buffer(_data[i], count),
                                 [this, self, count](boost::system::error_code ec, std::size_t ) {
                                     if (!ec) {
                                         search_server::SearchServer::addToLog("Request to\t" + getRemoteIP() + "\tcommand\t'" + getTextCommand(header_.command) +
                                                                               "'\tsize " +  std::to_string(count) + "\tbytes");
                                     } else {
                                         std::cerr << "Error on send: " << ec.message() << std::endl;
                                     }
                                 });
    }

    readHeader();
}*/





template<>
void asio_server::session::writeToSocket(const File<std::vector<BYTE>>& _data) {
    auto self(shared_from_this());

    try {
        boost::asio::post(socket_.get_executor(), [this, self, _data]() {
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                for (int i = 0; i < _data.getBlocksCount(); ++i) {
                    auto buffer = std::make_shared<std::vector<BYTE>>(_data[i]);
                    writeQueue.push(buffer);
                    search_server::SearchServer::addToLog("Queued buffer of size " + std::to_string(buffer->size()) + " for client: " + getRemoteIP());
                }
            }

            search_server::SearchServer::addToLog("Starting buffer processing for client: " + getRemoteIP());
            doWrite();
        });
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in writeToSocket: ") + e.what());
        socket_.close();
    }
}

void asio_server::session::writeToSocket(const std::filesystem::path& file_path)
{
    auto self(shared_from_this());

    try {
        boost::asio::post(socket_.get_executor(), [this, self, file_path]() {
            try {
                file_stream = std::make_unique<std::ifstream>(file_path, std::ios::binary);
                if (!file_stream->is_open()) {
                    search_server::SearchServer::addToLog("Не удалось открыть файл: " + file_path.string());
                    socket_.close();
                    return;
                }

                search_server::SearchServer::addToLog("Начинаем потоковую отправку: " + file_path.string());
                sendNextFileChunk(); // старт отправки
            }
            catch (const std::exception& ex) {
                search_server::SearchServer::addToLog(std::string("Ошибка при открытии файла: ") + ex.what());
                socket_.close();
            }
        });
    }
    catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in writeToSocket: ") + e.what());
        socket_.close();
    }
}

void asio_server::session::sendNextFileChunk()
{
    constexpr std::size_t blockSize = 64 * 1024;

    if (!file_stream || !file_stream->is_open()) {
        search_server::SearchServer::addToLog("Файл не открыт для отправки");
        return;
    }

    auto buffer = std::make_shared<std::vector<BYTE>>(blockSize);
    file_stream->read(reinterpret_cast<char*>(buffer->data()), blockSize);
    std::streamsize bytesRead = file_stream->gcount();

    if (bytesRead <= 0) {
        search_server::SearchServer::addToLog("Файл полностью отправлен");
        return;
    }

    buffer->resize(bytesRead);
    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        writeQueue.push(buffer);
    //    search_server::SearchServer::addToLog("Буфер размером " + std::to_string(bytesRead) + " добавлен в очередь отправки");
    }

    doWrite(); // отправляем этот блок

    // Постим следующее чтение
    auto self(shared_from_this());
    boost::asio::post(socket_.get_executor(), [this, self]() {
        sendNextFileChunk();
    });
}

void asio_server::session::doWrite() {
    auto self(shared_from_this());

    try {
     //   search_server::SearchServer::addToLog("Entering doWrite for client: " + getRemoteIP());

        std::lock_guard<std::mutex> lock(socket_mutex);
        if (isWriting || writeQueue.empty()) {
        //    search_server::SearchServer::addToLog("Write process already in progress or write queue is empty for client: " + getRemoteIP());
            return;
        }

        isWriting = true;
       // search_server::SearchServer::addToLog("Set isWriting to true for client: " + getRemoteIP());

        auto buffer = writeQueue.front();
        writeQueue.pop();

      //  search_server::SearchServer::addToLog("Writing buffer of size " + std::to_string(buffer->size()) + " for client: " + getRemoteIP());

        boost::asio::async_write(socket_, boost::asio::buffer(*buffer),
                                 [this, self, buffer](boost::system::error_code ec, std::size_t length) {
                                     try {
                                         if (!ec) {
                                    //         search_server::SearchServer::addToLog("Successfully wrote buffer of size " + std::to_string(length) + " for client: " + getRemoteIP());
                                             {
                                                 std::lock_guard<std::mutex> lock(socket_mutex);
                                                 isWriting = false;
                                 //                search_server::SearchServer::addToLog("Set isWriting to false for client: " + getRemoteIP());
                                             }
                                             doWrite();
                                         } else {
                                             std::cerr << "Error on send: " << ec.message() << std::endl;
                                             search_server::SearchServer::addToLog("Error on send: " + ec.message() + " for client: " + getRemoteIP());
                                             {
                                                 std::lock_guard<std::mutex> lock(socket_mutex);
                                                 isWriting = false;
                                        //         search_server::SearchServer::addToLog("Set isWriting to false due to error for client: " + getRemoteIP());
                                             }
                                             socket_.close();
                                         }
                                     } catch (const std::exception& e) {
                                         search_server::SearchServer::addToLog(std::string("Exception in async_write: ") + e.what());
                                         socket_.close();
                                     }
                                 });
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in doWrite: ") + e.what());
        socket_.close();
    }
}

template<>
[[maybe_unused]] void asio_server::session::writeToSocket(const File<std::string>& _data) {
    auto self(shared_from_this()); // Захват shared_ptr для гарантии времени жизни объекта

    for(int i = 0; i < _data.getBlocksCount(); i++) {
        auto count = _data[i].size();
        boost::asio::async_write(socket_, boost::asio::buffer(_data[i], count),
                                 [this, self, count](boost::system::error_code ec, std::size_t /*length*/) { // Захват self
                                     if (!ec) {
                                         search_server::SearchServer::addToLog("Request to\t" + getRemoteIP() + "\tcommand\t'" + getTextCommand(header_.command) +
                                                                               "'\tsize " + std::to_string(count) + "\tbytes");
                                     } else {
                                         std::cerr << "Error on send: " << ec.message() << std::endl; // Обработка ошибки отправки
                                     }
                                 });
    }
    readHeader(); // Переход к чтению заголовка после завершения записи
}


void asio_server::session::commandExec() {
    try {
        // Обработка команды
        std::vector<BYTE> answer;
        PersonalRequest personalRequest{};
        personalRequest.request_type = getTextCommand(header_.command);

        if (header_.command == COMMAND::GETBINFILE) {
            // answer содержит путь к файлу (в виде std::vector<BYTE>, но это строка пути)
            std::string pathStr(v_data_.begin(), v_data_.end());
            personalRequest.request = pathStr;
            std::filesystem::path file_path(pathStr);

            // Устанавливаем размер вручную — получим размер файла
            std::uintmax_t file_size = std::filesystem::file_size(file_path);
            header_.size = static_cast<std::size_t>(file_size);

            writeHeader();
            writeToSocket(file_path);
        }
        else
        {

            if (header_.command == COMMAND::USER_REGISTRY)
            {
                userName_ = std::string(v_data_.begin(), v_data_.end());
                std::string answer_str = "OK";
                answer = std::vector<BYTE>(answer_str.begin(), answer_str.end());
            }
            else
            {
                answer = Interface::execCommand(header_.command, v_data_);
            }
            auto request = std::string(v_data_.begin(), v_data_.end());
            if(header_.command == COMMAND::LOAD_TLG_TO_SEND || header_.command == COMMAND::LOAD_RAZN)
                personalRequest.request = getTextCommand(header_.command);
            else
                personalRequest.request  = request.empty() ? "EMPTY" : request;


            header_.size = answer.size();
            writeHeader();
            writeToSocket(File<std::vector<BYTE>>(answer));
        }

        personalRequest.user_name = userName_;
        logutil::log(personalRequest);

        v_data_.clear();
        readHeader();
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in commandExec: ") + e.what());
        socket_.close();
    }
}



void asio_server::session::writeHeader() {
    auto self(shared_from_this());
    try {
        boost::asio::async_write(socket_, boost::asio::buffer(&header_, sizeof(Header)),
                                 [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                                     if (!ec) {
                                         if (header_.command == COMMAND::SOMEERROR) {
                                             socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                                             socket_.close(ec);
                                             header_ = Header{};
                                         }
                                     } else {
                                         search_server::SearchServer::addToLog("Error on write header: " + ec.message());
                                         socket_.close();
                                     }
                                 });
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in writeHeader: ") + e.what());
        socket_.close();
    }
}


std::string asio_server::session::getRemoteIP() const {
    boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint();
    boost::asio::ip::address remote_ad = remote_ep.address();

    return remote_ad.to_string();
}

void asio_server::session::writeToSocket(const std::string& str) {
    auto self(shared_from_this()); // Захват shared_ptr для гарантии времени жизни объекта

    boost::asio::async_write(socket_, boost::asio::buffer(str, str.size()),
                             [this, self](boost::system::error_code ec, std::size_t /*length*/) { // Захват self
                                 if (!ec) {
                                     search_server::SearchServer::addToLog("Request to\t" + getRemoteIP() + "\tcommand\t'" + getTextCommand(header_.command) +
                                                                           "'\tsize " + std::to_string(header_.size) + "\tbytes");

                                     readHeader(); // Переход к чтению заголовка после завершения записи
                                 } else {
                                     std::cerr << "Error on send: " << ec.message() << std::endl; // Обработка ошибки отправки
                                 }
                             });
}


bool asio_server::session::trustCommand() {
    try {
        if (header_.command > COMMAND::SOMEERROR && header_.command < COMMAND::END_COMMAND)
            return true;

        if (static_cast<COMMAND>(static_cast<uint64_t>(header_.command) >> 32) == COMMAND::SAVE_MESSAGE_TO) {
            userId_ = static_cast<uint32_t>(static_cast<uint64_t>(header_.command) & 0xFFFFFFFF);
            header_.command = COMMAND::SAVE_MESSAGE_TO;
            Interface::setCommand(COMMAND::SAVE_MESSAGE_TO, std::make_unique<SaveMessageCmd>(userId_));
            return true;
        }

        header_.command = COMMAND::SOMEERROR;
        return false;
    } catch (const std::exception& e) {
        search_server::SearchServer::addToLog(std::string("Exception in trustCommand: ") + e.what());
        header_.command = COMMAND::SOMEERROR;
        return false;
    }
}

std::string asio_server::getTextCommand(COMMAND command) {


    static const std::unordered_map<COMMAND, std::string> commandMap = {
            {COMMAND::JSONREGUEST, "JSON request"},
            {COMMAND::FILETEXT, "FILE text"},
            {COMMAND::SOLOREQUEST, "SINGLE request"},
            {COMMAND::ADDRESOLUTION, "ADD_RESOLUTION"},
            {COMMAND::UPDATE, "UPDATE"},
            {COMMAND::GETBINFILE, "GET_BIN_FILE"},
            {COMMAND::GETRESOLUTION, "GET_RESOLUTION"},
            {COMMAND::LOAD_TLG_TO_SEND, "LOAD_TLG_TO_SEND"},
            {COMMAND::SOMEERROR, "ERROR :("},
            {COMMAND::GETRESOLUTIONS, "GET_RESOLUTIONS"},
            {COMMAND::GETDOCS, "GET_DOCUMENTS"},
            {COMMAND::GETDOC, "GET_DOCUMENT"},
            {COMMAND::GETSQLJSONANSWEAR, "GET_SQL_JSON_ANSWER"},
            {COMMAND::GET_VH_TELEGI_FROM_SQL, "GET_INCOMING_TELEGRAMS_FROM_SQL"},
            {COMMAND::GET_ISH_TELEGI_FROM_SQL, "GET_OUTGOING_TELEGRAMS_FROM_SQL"},
            {COMMAND::START_UPDATE_BASE, "START_UPDATE_BASE"},
            {COMMAND::SAVE_MESSAGE_TO, "SAVE_MESSAGE_TO"},
            {COMMAND::END_COMMAND, "END"},
            {COMMAND::GET_MESSAGE, "GET_MESSAGE"},
            {COMMAND::USER_REGISTRY, "AUTHORIZATION"},
            {COMMAND::LOAD_RAZN, "LOAD_RAZN"},
            {COMMAND::GET_OPIS_BASE, "GET_OPIS_BASE"},
            {COMMAND::PING, "PING"}
    };


    auto it = commandMap.find(command);
        return it != commandMap.end() ? it->second : "UNKNOWN COMMAND";
}



void asio_server::Interface::setSearchServer(search_server::SearchServer *_server) {
    searchServer_ = _server;

    cmdMap[COMMAND::SOLOREQUEST] = std::make_unique<SoloRequestCmd>(searchServer_);
    cmdMap[COMMAND::LOAD_TLG_TO_SEND] = std::make_unique<SaveTlgToSendCmd>(L"D:\\");
    cmdMap[COMMAND::LOAD_RAZN] = std::make_unique<SaveFileDefaultCmd>("D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ\\");
    cmdMap[COMMAND::FILETEXT] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>& v){ return GetFileCmd::downloadFileByPath(v);});
    cmdMap[COMMAND::GETBINFILE] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>& v){ return GetFileCmd::downloadFileByPath(v);});
    cmdMap[COMMAND::GET_VH_TELEGI_FROM_SQL] = std::make_unique<GetJsonTelegaVhCmd>();
    cmdMap[COMMAND::GET_ISH_TELEGI_FROM_SQL] = std::make_unique<GetJsonTelegaIshCmd>();
    cmdMap[COMMAND::GETSQLJSONANSWEAR] = std::make_unique<GetSqlJsonAnswearCmd>(searchServer_);
    cmdMap[COMMAND::START_UPDATE_BASE] = std::make_unique<StartDictionaryUpdateCmd>(searchServer_);
    cmdMap[COMMAND::USER_REGISTRY] = std::make_unique<RegisterUserCmd>();
    cmdMap[COMMAND::PING] = std::make_unique<PingCmd>();
    cmdMap[COMMAND::GET_MESSAGE] = std::make_unique<GetMessageCmd>();
    cmdMap[COMMAND::GET_ISH_TELEGA_WAY] = std::make_unique<GetTelegaWayIshCmd>();
    cmdMap[COMMAND::GET_VH_TELEGA_WAY] = std::make_unique<GetTelegaWayVhCmd>();
    cmdMap[COMMAND::GET_OPIS_BASE] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>&){ return GetFileCmd::downloadFileByPath("D:\\OPIS_ADMIN\\" + year_ + ".db"); });
    cmdMap[COMMAND::GET_ATTACHMENTS] = std::make_unique<GetAttachmentsCmd>();
}

std::vector<uint8_t> asio_server::Interface::execCommand(asio_server::COMMAND _command, std::vector<uint8_t> &_request) {
    return cmdMap.at(_command)->execute(_request);
}

void asio_server::Interface::setCommand(asio_server::COMMAND _command, std::unique_ptr<Command> _myCmd) {
    cmdMap[_command] = std::move(_myCmd);
}

void asio_server::Interface::setYear(const std::string &_year) {
    year_ = _year;
}

std::string asio_server::Interface::getYear() {
   return year_;
}

void asio_server::AsioServer::do_accept() {
    acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec)
                    std::make_shared<session>(std::move(socket))->start();

                do_accept();
            });
}

asio_server::AsioServer::AsioServer(boost::asio::io_context &io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    mySql::init("JURNAL.db3");
    do_accept();
}
