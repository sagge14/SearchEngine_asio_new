#include "AsioServer.h"
#include "SearchServer/SearchServer.h"
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
#include "Commands/GetIshTelegaPdtv/GetIshTelegaPdtvCommand.h"
#include "Commands/GetTelegaAttachments/GetTelegaAttachments.h"
#include "Commands/GetTelegaSingleAttachment/GetTelegaSingleAttachmentCmd.h"
#include <mutex>

using boost::asio::ip::tcp;

void asio_server::session::start() {

    auto self = shared_from_this();
    boost::asio::co_spawn(
            socket_.get_executor(),
            [self]() -> boost::asio::awaitable<void> {

                co_await self->readLoop();
            },
            boost::asio::detached
    );

    boost::asio::co_spawn(
            socket_.get_executor(),
            [self]() -> boost::asio::awaitable<void> {
                co_await self->writeLoop();
            },
            boost::asio::detached
    );

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

boost::asio::awaitable<void> asio_server::session::readLoop() {
    try {
        std::cout << "Connect\t\t" + getRemoteIP() << std::endl;
        search_server::addToLog("Connect\t\t" + getRemoteIP());

        while (socket_.is_open()) {
            // --- Чтение заголовка ---
            co_await boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(header_)), boost::asio::use_awaitable);

            if (!trustCommand()) {
                header_.command = COMMAND::SOMEERROR;
                search_server::addToLog("Received from\t" + getRemoteIP() + "\tcommand\t'" + getTextCommand(header_.command) + "'");
                commandExec();
                co_return;
            }

            std::cout << "Header read\t" + getRemoteIP() << std::endl;
            search_server::addToLog("Header read\t" + getRemoteIP());

            v_data_.clear();
            v_data_.resize(header_.size);

            std::size_t total_read = 0;
            while (total_read < header_.size) {
                std::size_t to_read = std::min(static_cast<size_t>(max_length), static_cast<size_t>(header_.size - total_read));
                std::size_t bytes = co_await socket_.async_read_some(
                        boost::asio::buffer(v_data_.data() + total_read, to_read), boost::asio::use_awaitable);

                total_read += bytes;
            }

            std::cout << "Received all data. Processing..." << std::endl;
            commandExec();
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() == boost::asio::error::eof) {
            search_server::addToLog("Клиент закрыл соединение (" + getRemoteIP() + ")");
        } else {
            search_server::addToLog(std::string("Ошибка в readLoop: ") + e.what());
        }

        socket_.close();
        logutil::log(userName_,"EMPTY","DISCONNECT");
    }

}

boost::asio::awaitable<void> asio_server::session::sendNextFileChunk() {

    constexpr std::size_t blockSize = 64 * 1024;

    if (!socket_.is_open()) {
        search_server::addToLog("Соединение прервано, отправка отменена");
        co_return;
    }

    if (!file_stream || !file_stream->is_open()) {
        search_server::addToLog("Файл не открыт для отправки");
        co_return ;
    }

    while(file_stream && file_stream->is_open()) {


        auto buffer = std::make_shared<std::vector<BYTE>>(blockSize);
        file_stream->read(reinterpret_cast<char *>(buffer->data()), blockSize);
        std::streamsize bytesRead = file_stream->gcount();

        if (bytesRead <= 0) {
            search_server::addToLog("Файл полностью отправлен");
            co_return;
        }

        buffer->resize(bytesRead);

        boost::system::error_code ec;
        co_await boost::asio::async_write(socket_, boost::asio::buffer(*buffer), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            search_server::addToLog("Ошибка при отправке: " + ec.message());
            socket_.close(); // <--- обязательно!
            logutil::log(userName_,"EMPTY","DISCONNECT");
            co_return;
        }

    }
}

void  asio_server::session::commandExec() {
    try {

        // Обработка команды
        std::vector<BYTE> answer;
        PersonalRequest personalRequest{};
        personalRequest.request_type = getTextCommand(header_.command);

        if (header_.command == COMMAND::SOMEERROR)
        {
            write_channel_.try_send(boost::system::error_code{}, header_); // сначала заголовок
            return;
        }

        if (header_.command == COMMAND::GETBINFILE)
        {
            // answer содержит путь к файлу (в виде std::vector<BYTE>, но это строка пути)
            std::string pathStr(v_data_.begin(), v_data_.end());
            personalRequest.request = pathStr;
            std::filesystem::path file_path(pathStr);

            // Устанавливаем размер вручную — получим размер файла

            std::uintmax_t file_size;
            if(!exists(file_path))
            {
                header_.command = COMMAND::SOMEERROR;
                write_channel_.try_send(boost::system::error_code{}, header_); // сначала заголовок
            }
            else
            {

                file_size = std::filesystem::file_size(file_path);
                header_.size = static_cast<std::size_t>(file_size);

                write_channel_.try_send(boost::system::error_code{}, header_); // сначала заголовок
                write_channel_.try_send(boost::system::error_code{}, file_path);
            }

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

            write_channel_.try_send(boost::system::error_code{}, header_); // сначала заголовок
            write_channel_.try_send(boost::system::error_code{}, std::make_shared<std::vector<BYTE>>(answer)); // потом сами данные

        }

        personalRequest.user_name = userName_;
        logutil::log(personalRequest);

        v_data_.clear();
    } catch (const std::exception& e) {
        search_server::addToLog(std::string("Exception in commandExec: ") + e.what());
      //  header_.command = COMMAND::SOMEERROR;
        //write_channel_.try_send(boost::system::error_code{}, header_); // сначала заголовок

       // socket_.close();
       // logutil::log(userName_,"EMPTY","DISCONNECT");
    }
}

std::string asio_server::session::getRemoteIP() const {
    boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint();
    boost::asio::ip::address remote_ad = remote_ep.address();

    return remote_ad.to_string();
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
        search_server::addToLog(std::string("Exception in trustCommand: ") + e.what());
        header_.command = COMMAND::SOMEERROR;
        return false;
    }
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
    cmdMap[COMMAND::GET_ISH_PDTV] = std::make_unique<GetIshTelegaPdtvCommand>();
    cmdMap[COMMAND::GET_TELEGA_ATACHMENTS] = std::make_unique<GetTelegaAttachmentsCmd>();
    cmdMap[COMMAND::GET_SINGLE_ATACHMENT] = std::make_unique<GetTelegaSingleAttachmentCmd>();
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

boost::asio::awaitable<void> asio_server::session::writeLoop() {
    try {
        while (socket_.is_open()) {
            boost::system::error_code ec;
            WriteItem item;
            std::tie(ec, item) = co_await write_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (ec) break;

            if (std::holds_alternative<asio_server::Header>(item)) {
                const asio_server::Header& hdr = std::get<asio_server::Header>(item);
                co_await boost::asio::async_write(socket_, boost::asio::buffer(&hdr, sizeof(hdr)), boost::asio::use_awaitable);

                if (hdr.command == COMMAND::SOMEERROR) {
                 //   write_channel_.close();
                //    socket_.close();
                 //   logutil::log(userName_,"EMPTY","DISCONNECT");
                    co_return; // или break
                }

            } else if (std::holds_alternative<std::shared_ptr<std::vector<BYTE>>>(item)) {
                auto buf = std::get<std::shared_ptr<std::vector<BYTE>>>(item);
                co_await boost::asio::async_write(socket_, boost::asio::buffer(*buf), boost::asio::use_awaitable);
            } else if (std::holds_alternative<std::filesystem::path>(item)) {
                if(co_await openFileStream(std::get<std::filesystem::path>(item)))
                    co_await sendNextFileChunk();
            }
        }
    } catch (const std::exception& e) {
        search_server::addToLog(std::string("Exception in writeLoop: ") + e.what());
        socket_.close(); // <--- обязательно!
        logutil::log(userName_,"EMPTY","DISCONNECT");
    }
}

boost::asio::awaitable<bool> asio_server::session::openFileStream(const std::filesystem::path& file_path)
{
    try {
        file_stream = std::make_unique<std::ifstream>(file_path, std::ios::binary);
        if (!file_stream->is_open()) {
            search_server::addToLog("Не удалось открыть файл: " + file_path.string());
          //  socket_.close();
            co_return false;
        }
        search_server::addToLog("Начинаем потоковую отправку: " + file_path.string());
        co_return true;
    }
    catch (const std::exception& ex) {
        search_server::addToLog(std::string("Ошибка при открытии файла: ") + ex.what());
       // socket_.close();
        co_return false;
    }

}

template<typename T>
bool asio_server::session::safe_send_to_channel(T&& item) {
    if (!write_channel_.try_send(boost::system::error_code{}, std::forward<T>(item))) {
        asio_server::Header error_header;
        error_header.command = asio_server::COMMAND::SOMEERROR;
        error_header.size = 0;
        write_channel_.try_send(boost::system::error_code{}, error_header);
        return false;
    }
    return true;
}