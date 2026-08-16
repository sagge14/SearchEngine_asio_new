#include "AsioServer.h"
#include "SearchServer/SearchServer.h"
#include "SQLite/SQLiteConnectionManager.h"
#include "MyUtils/SqlLogger.h"
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
#include "Commands/Auth/AuthenticateCmd.h"
#include "Auth/AuthRuntime.h"
#include <nlohmann/json.hpp>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <boost/concept_check.hpp>

using boost::asio::ip::tcp;

bool asio_server::ServerActivity::tryStartCommand()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping)
        return false;
    ++active_commands;
    return true;
}

void asio_server::ServerActivity::finishCommand() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (active_commands > 0)
        --active_commands;
    if (active_commands == 0 && active_sessions == 0)
        condition.notify_all();
}

bool asio_server::ServerActivity::tryStartSession()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping)
        return false;
    ++active_sessions;
    return true;
}

void asio_server::ServerActivity::finishSession() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    if (active_sessions > 0)
        --active_sessions;
    if (active_commands == 0 && active_sessions == 0)
        condition.notify_all();
}

void asio_server::ServerActivity::stopAccepting() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
    if (active_commands == 0 && active_sessions == 0)
        condition.notify_all();
}

void asio_server::ServerActivity::wait()
{
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this]() {
        return active_commands == 0 && active_sessions == 0;
    });
}

void asio_server::session::start()
{
    remoteIP_ = getRemoteIP();
    auto self = shared_from_this();
    auto ex = strand_;

    boost::asio::co_spawn(
            ex,
            [self]() -> boost::asio::awaitable<void> {
                co_await self->readLoop();
            },
            [self](const std::exception_ptr& eptr) {
                if (eptr) {
                    try { std::rethrow_exception(eptr); }
                    catch (const std::exception& e) {
                        search_server::addToLog(std::string("readLoop failed: ") + e.what());
                    }
                }
                if (!self->terminal_error_queued_.load(std::memory_order_acquire))
                    self->stop("readLoop finished");
            }
    );

    boost::asio::co_spawn(
            ex,
            [self]() -> boost::asio::awaitable<void> {
                co_await self->writeLoop();
            },
            [self](const std::exception_ptr& eptr) {
                if (eptr) {
                    try { std::rethrow_exception(eptr); }
                    catch (const std::exception& e) {
                        search_server::addToLog(std::string("writeLoop failed: ") + e.what());
                    }
                }
                self->stop("writeLoop finished");
            }
    );
}


boost::asio::awaitable<void> asio_server::session::readLoop()
{
    try {
        search_server::addToLog("Connect\t\t" + getRemoteIP());

        while (!stopped_ && socket_.is_open()) {

            Header requestHeader{};
            co_await boost::asio::async_read(
                    socket_, boost::asio::buffer(&requestHeader, sizeof(requestHeader)),
                    boost::asio::use_awaitable);

            // 1) лимит размера
            constexpr std::size_t MAX_PAYLOAD = 1000 * 1024 * 1024; // 1GB
            if (requestHeader.size > MAX_PAYLOAD) {
                co_await queueError(
                    command_execution::ErrorCode::PayloadTooLarge,
                    "payload_size=" + std::to_string(requestHeader.size),
                    true,
                    requestHeader.command);
                co_return;
            }

            // 2) валидация команды
            std::optional<uint_fast32_t> saveMessageUserId;
            if (!trustCommand(requestHeader, saveMessageUserId)) {
                co_await queueError(
                    command_execution::ErrorCode::InvalidCommand,
                    "wire_command=" + std::to_string(
                        static_cast<uint_fast64_t>(requestHeader.command)),
                    true,
                    requestHeader.command);
                co_return;
            }

            std::vector<BYTE> requestData(requestHeader.size, 0);

            std::size_t total_read = 0;
            while (total_read < requestHeader.size) {
                std::size_t to_read = std::min<std::size_t>(
                    max_length,
                    requestHeader.size - total_read);
                std::size_t bytes = co_await socket_.async_read_some(
                        boost::asio::buffer(requestData.data() + total_read, to_read),
                        boost::asio::use_awaitable);
                total_read += bytes;
            }

            if (!activity_ || !activity_->tryStartCommand()) {
                co_await queueError(
                    command_execution::ErrorCode::ServerStopping,
                    {},
                    true,
                    requestHeader.command);
                co_return;
            }

            try {
                co_await co_spawn(
                    cpu_pool_,
                    [self = shared_from_this(),
                     requestHeader,
                     requestData = std::move(requestData),
                     saveMessageUserId]() mutable
                        -> boost::asio::awaitable<void>
                    {
                        co_await self->commandExec(
                            requestHeader,
                            std::move(requestData),
                            saveMessageUserId);
                        co_return;
                    },
                    boost::asio::use_awaitable);
            } catch (const std::exception& e) {
                search_server::addToLog(
                    std::string("commandExec failed: ") + e.what());
            } catch (...) {
                search_server::addToLog(
                    "commandExec failed: unknown exception");
            }

            activity_->finishCommand();

        }
    }
    catch (const boost::system::system_error& e) {
        if (e.code() == boost::asio::error::eof)
            search_server::addToLog("Client closed (" + getRemoteIP() + ")");
        else
            search_server::addToLog(std::string("readLoop error: ") + e.what());
    }

    co_return;
}

void asio_server::session::stop(const char* why)
{
    auto self = shared_from_this();
    boost::asio::dispatch(
        strand_,
        [self, reason = std::string(why ? why : "unspecified")]() {
            self->stopOnExecutor(reason);
        }
    );
}

void asio_server::session::stopOnExecutor(const std::string& why)
{
    // идемпотентно
    if (stopped_.exchange(true)) return;

    search_server::addToLog("Session stop: " + why + " " + getRemoteIP());

    boost::system::error_code ec;

    // 1) закрыть канал, чтобы writeLoop вышел из async_receive
    write_channel_.close();

    // 2) закрыть сокет
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    // 3) логировать DISCONNECT один раз
    std::string userName;
    {
        std::lock_guard<std::mutex> lock(user_name_mutex_);
        userName = userName_;
    }
    logutil::log(userName, "EMPTY", "DISCONNECT");
    finishSession();
}


boost::asio::awaitable<void> asio_server::session::writeLoop()
{
    try {
        while (!stopped_ && socket_.is_open()) {
            boost::system::error_code ec;
            WriteItem item;

            std::tie(ec, item) =
                    co_await write_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));

            if (ec) break; // channel closed => выходим

            if (auto response = std::get_if<ResponseWrite>(&item)) {
                co_await boost::asio::async_write(
                    socket_,
                    boost::asio::buffer(&response->header, sizeof(response->header)),
                    boost::asio::use_awaitable);
                if (response->payload && !response->payload->empty()) {
                    co_await boost::asio::async_write(
                        socket_,
                        boost::asio::buffer(*response->payload),
                        boost::asio::use_awaitable);
                }
            }
            else if (auto file = std::get_if<FileTransfer>(&item)) {
                co_await boost::asio::async_write(
                    socket_,
                    boost::asio::buffer(&file->header, sizeof(file->header)),
                    boost::asio::use_awaitable);
                if (!(co_await sendFile(file->stream, file->header.size))) {
                    stop("file transfer failed");
                    co_return;
                }
            }
            else if (auto error = std::get_if<ErrorWrite>(&item)) {
                co_await boost::asio::async_write(
                    socket_,
                    boost::asio::buffer(&error->header, sizeof(error->header)),
                    boost::asio::use_awaitable);
                if (error->payload && !error->payload->empty()) {
                    co_await boost::asio::async_write(
                        socket_,
                        boost::asio::buffer(*error->payload),
                        boost::asio::use_awaitable);
                }
                if (error->closeAfterWrite) {
                    stop("terminal error sent");
                    co_return;
                }
            }
        }
    }
    catch (const std::exception& e) {
        search_server::addToLog(std::string("writeLoop exception: ") + e.what());
    }

    co_return;
}


asio_server::session::~session()
{
    finishSession();
    boost::system::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
}


boost::asio::awaitable<bool> asio_server::session::sendFile(
    const std::shared_ptr<std::ifstream>& stream,
    uint_fast64_t expectedSize)
{
    constexpr std::size_t blockSize = 64 * 1024;

    if (!socket_.is_open()) {
        search_server::addToLog("Соединение прервано, отправка отменена");
        co_return false;
    }

    if (!stream || !stream->is_open()) {
        search_server::addToLog("Файл не открыт для отправки");
        co_return false;
    }

    uint_fast64_t bytesRemaining = expectedSize;
    while (bytesRemaining > 0) {
        const auto nextSize = static_cast<std::size_t>(
            std::min<uint_fast64_t>(blockSize, bytesRemaining));
        auto buffer = std::make_shared<std::vector<BYTE>>(nextSize);
        stream->read(
            reinterpret_cast<char*>(buffer->data()),
            static_cast<std::streamsize>(nextSize));
        const std::streamsize bytesRead = stream->gcount();

        if (bytesRead <= 0) {
            search_server::addToLog("File read failed before advertised size");
            co_return false;
        }

        buffer->resize(static_cast<std::size_t>(bytesRead));

        boost::system::error_code ec;
        co_await boost::asio::async_write(
                socket_,
                boost::asio::buffer(*buffer),
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            search_server::addToLog("sendNextFileChunk socket error: " + ec.message());
            co_return false;
        }

        bytesRemaining -= static_cast<uint_fast64_t>(bytesRead);
    }

    search_server::addToLog("Файл полностью отправлен");
    co_return true;
}

boost::asio::awaitable<bool> asio_server::session::queueError(
    command_execution::ErrorCode error,
    std::string diagnostic,
    bool closeAfterWrite,
    COMMAND requestCommand)
{
    constexpr std::size_t maxDiagnosticSize = 512;
    if (diagnostic.size() > maxDiagnosticSize) {
        diagnostic.resize(maxDiagnosticSize);
        diagnostic += "...";
    }
    for (char& character : diagnostic) {
        if (character == '\r' || character == '\n' || character == '\t')
            character = ' ';
    }

    const bool typedResponse =
        typed_errors_enabled_.load(std::memory_order_acquire);

    std::string logMessage =
        "Command error: code=" + std::string(command_execution::toString(error)) +
        ", command=" + getTextCommand(requestCommand) +
        ", remote=" + getRemoteIP() +
        ", wire=" + (typedResponse ? "typed-v1" : "legacy");
    if (!diagnostic.empty())
        logMessage += ", diagnostic=" + diagnostic;
    search_server::addToLog(logMessage);

    boost::system::error_code sendError;
    Header errorHeader{};
    auto errorPayload = std::make_shared<std::vector<BYTE>>();
    if (typedResponse) {
        const auto wireError = makeTypedErrorResponse(error);
        errorHeader = Header{
            sizeof(wireError),
            COMMAND::ERROR_RESPONSE};
        errorPayload->resize(sizeof(wireError));
        std::memcpy(errorPayload->data(), &wireError, sizeof(wireError));
    } else {
        errorHeader = makeLegacyErrorHeader(error);
    }

    co_await write_channel_.async_send(
        boost::system::error_code{},
        WriteItem{ErrorWrite{errorHeader, std::move(errorPayload), closeAfterWrite}},
        boost::asio::redirect_error(boost::asio::use_awaitable, sendError));

    if (sendError) {
        search_server::addToLog(
            "Failed to queue command error " +
            std::string(command_execution::toString(error)) +
            ": " + sendError.message());
        co_return false;
    }

    if (closeAfterWrite)
        terminal_error_queued_.store(true, std::memory_order_release);

    co_return true;
}

boost::asio::awaitable<void> asio_server::session::commandExec(
    Header requestHeader,
    std::vector<BYTE> requestData,
    std::optional<uint_fast32_t> saveMessageUserId)
{
    std::optional<command_execution::CommandResult> unexpectedFailure;
    bool responseQueued = false;

    try {
        // Обработка команды
        std::vector<BYTE> answer;
        bool enableTypedErrorsAfterResponse = false;
        PersonalRequest personalRequest{};
        personalRequest.request_type = getTextCommand(requestHeader.command);
        boost::system::error_code ec;

        /*
        boost::asio::steady_timer timer(
                co_await boost::asio::this_coro::executor
        );

        timer.expires_after(5s);   // ← сколько держать сервер занятым
        co_await timer.async_wait(boost::asio::use_awaitable);
         */

        if (requestHeader.command == COMMAND::SOMEERROR)
        {
            co_await queueError(
                command_execution::ErrorCode::InvalidCommand,
                {},
                false,
                requestHeader.command);
            co_return;
        }

        // Session authorization gate: data commands require
        // USER_REGISTRY("admin") from TCP peer 127.0.0.1, or successful
        // AUTHENTICATE_V1 (any peer). Unauthenticated data access fails closed.
        const auto gate = evaluateSessionCommandGate(
            requestHeader.command,
            authenticated_);
        if (!gate.allow_execute)
        {
            co_await queueError(
                command_execution::ErrorCode::AuthRequired,
                "session is not authenticated",
                gate.close_after_auth_required,
                requestHeader.command);
            co_return;
        }

        if (requestHeader.command == COMMAND::GETBINFILE)
        {
            // answer содержит путь к файлу (в виде std::vector<BYTE>, но это строка пути)
            std::string pathStr(requestData.begin(), requestData.end());
            personalRequest.request = pathStr;
            std::filesystem::path file_path(pathStr);

            std::error_code fileError;
            const bool fileExists = std::filesystem::exists(file_path, fileError);
            if (fileError) {
                co_await queueError(
                    command_execution::ErrorCode::FileMetadataFailed,
                    fileError.message(),
                    false,
                    requestHeader.command);
                co_return;
            }
            if (!fileExists) {
                co_await queueError(
                    command_execution::ErrorCode::FileNotFound,
                    file_path.string(),
                    false,
                    requestHeader.command);
                co_return;
            }

            auto fileStream = std::make_shared<std::ifstream>(
                file_path,
                std::ios::binary);
            if (!fileStream->is_open()) {
                co_await queueError(
                    command_execution::ErrorCode::FileOpenFailed,
                    file_path.string(),
                    false,
                    requestHeader.command);
                co_return;
            }

            fileStream->seekg(0, std::ios::end);
            const auto endPosition = fileStream->tellg();
            const auto endOffset = static_cast<std::streamoff>(endPosition);
            fileStream->seekg(0, std::ios::beg);
            if (endOffset < 0 || !*fileStream) {
                co_await queueError(
                    command_execution::ErrorCode::FileMetadataFailed,
                    file_path.string(),
                    false,
                    requestHeader.command);
                co_return;
            }

            requestHeader.size = static_cast<uint_fast64_t>(endOffset);

            co_await write_channel_.async_send(
                ec,
                WriteItem{FileTransfer{requestHeader, std::move(fileStream)}},
                boost::asio::use_awaitable);
            responseQueued = true;
        }
        else
        {

            if (requestHeader.command == COMMAND::NEGOTIATE_PROTOCOL_V1)
            {
                search_protocol::ProtocolCapabilitiesV1 capabilities{};
                capabilities.capabilities =
                    search_protocol::CAPABILITY_TYPED_ERRORS_V1 |
                    search_protocol::CAPABILITY_CLIENT_AUTH_V1;
                answer.resize(sizeof(capabilities));
                std::memcpy(answer.data(), &capabilities, sizeof(capabilities));
                enableTypedErrorsAfterResponse = true;
            }
            else if (requestHeader.command == COMMAND::USER_REGISTRY)
            {
                // Legacy admin: payload "admin" AND TCP peer exactly 127.0.0.1.
                // Arbitrary usernames and non-localhost peers must not authorize.
                const std::string requestedName(
                    requestData.begin(),
                    requestData.end());
                if (!isLegacyAdminUserRegistryPayload(requestedName)) {
                    co_await queueError(
                        command_execution::ErrorCode::AuthFailed,
                        "USER_REGISTRY allows only legacy admin session",
                        true,
                        requestHeader.command);
                    co_return;
                }
                if (!isLocalAdminPeer()) {
                    search_server::addToLog(
                        "legacy admin authorization rejected: remote peer is not 127.0.0.1, remote="
                        + getRemoteIP());
                    co_await queueError(
                        command_execution::ErrorCode::AuthFailed,
                        "legacy admin authorization requires TCP peer 127.0.0.1",
                        true,
                        requestHeader.command);
                    co_return;
                }

                std::lock_guard<std::mutex> lock(user_name_mutex_);
                userName_ = "admin";
                authenticated_ = true;
                const std::string answer_str = "OK";
                answer = std::vector<BYTE>(answer_str.begin(), answer_str.end());
            }
            else if (requestHeader.command == COMMAND::AUTHENTICATE_V1)
            {
                // Failed AUTHENTICATE_V1 is terminal: send ERROR_RESPONSE, then
                // close. The client must not keep a half-open unauthenticated
                // TCP session after an explicit auth rejection.
                if (!auth::AuthRuntime::instance().isInitialized()) {
                    co_await queueError(
                        command_execution::ErrorCode::ConfigurationError,
                        "AuthClientStore is not initialized",
                        true,
                        requestHeader.command);
                    co_return;
                }

                AuthenticateCmd command(
                    auth::AuthRuntime::instance().store(),
                    auth::AuthRuntime::instance().verifier());
                auto result = command.executeResult(requestData);
                if (result.failed()) {
                    co_await queueError(
                        *result.error,
                        std::move(result.diagnostic),
                        true,
                        requestHeader.command);
                    co_return;
                }

                std::optional<command_execution::CommandResult> authSessionError;
                try {
                    const nlohmann::json response = nlohmann::json::parse(
                        result.payload.begin(),
                        result.payload.end());
                    const auto client_name =
                        response.at("client_name").get<std::string>();
                    const auto client_id =
                        response.at("client_id").get<std::string>();
                    std::string flash_serial;
                    try {
                        const nlohmann::json request = nlohmann::json::parse(
                            requestData.begin(),
                            requestData.end());
                        if (request.contains("flash_serial") &&
                            request.at("flash_serial").is_string())
                        {
                            flash_serial =
                                request.at("flash_serial").get<std::string>();
                        }
                    } catch (...) {
                    }
                    std::lock_guard<std::mutex> lock(user_name_mutex_);
                    userName_ = client_name;
                    clientId_ = client_id;
                    flashSerial_ = std::move(flash_serial);
                    authenticated_ = true;
                } catch (const std::exception& ex) {
                    authSessionError = command_execution::CommandResult::failure(
                        command_execution::ErrorCode::SerializationFailed,
                        std::string("AUTHENTICATE_V1 response parse failed: ") +
                            ex.what());
                }

                if (authSessionError && authSessionError->error) {
                    co_await queueError(
                        *authSessionError->error,
                        std::move(authSessionError->diagnostic),
                        true,
                        requestHeader.command);
                    co_return;
                }

                answer = std::move(result.payload);
            }
            else if (requestHeader.command == COMMAND::SAVE_MESSAGE_TO &&
                     saveMessageUserId)
            {
                // LEGACY: поддержка старого специального wire-маршрута.
                SaveMessageCmd command(*saveMessageUserId);
                auto result = command.executeResult(requestData);
                if (result.failed()) {
                    co_await queueError(
                        *result.error,
                        std::move(result.diagnostic),
                        false,
                        requestHeader.command);
                    co_return;
                }
                answer = std::move(result.payload);
            }
            else
            {
                auto result = Interface::execCommand(
                    requestHeader.command,
                    requestData);
                if (result.failed()) {
                    co_await queueError(
                        *result.error,
                        std::move(result.diagnostic),
                        false,
                        requestHeader.command);
                    co_return;
                }
                answer = std::move(result.payload);
            }

            auto request = std::string(requestData.begin(), requestData.end());
            if (requestHeader.command == COMMAND::LOAD_TLG_TO_SEND ||
                requestHeader.command == COMMAND::LOAD_RAZN)
                personalRequest.request = getTextCommand(requestHeader.command);
            else
                personalRequest.request  = request.empty() ? "EMPTY" : request;

            requestHeader.size = answer.size();

            co_await write_channel_.async_send(
                ec,
                WriteItem{ResponseWrite{
                    requestHeader,
                    std::make_shared<std::vector<BYTE>>(std::move(answer))}},
                boost::asio::use_awaitable);
            responseQueued = true;
            if (enableTypedErrorsAfterResponse)
                typed_errors_enabled_.store(true, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(user_name_mutex_);
            personalRequest.user_name = userName_;
        }
        logutil::log(personalRequest);

    } catch (const std::exception& e) {
        if (responseQueued) {
            search_server::addToLog(
                std::string("Post-response command failure: ") + e.what());
        } else {
            unexpectedFailure = command_execution::CommandResult::failure(
                command_execution::ErrorCode::InternalError,
                e.what());
        }
    } catch (...) {
        if (responseQueued) {
            search_server::addToLog("Unknown post-response command failure");
        } else {
            unexpectedFailure = command_execution::CommandResult::failure(
                command_execution::ErrorCode::InternalError,
                "unknown exception in commandExec");
        }
    }

    if (unexpectedFailure && unexpectedFailure->error) {
        co_await queueError(
            *unexpectedFailure->error,
            std::move(unexpectedFailure->diagnostic),
            false,
            requestHeader.command);
    }
}

bool asio_server::session::isLocalAdminPeer() const
{
    // Fail closed: any remote_endpoint error/exception means not local admin.
    try {
        boost::system::error_code ec;
        const boost::asio::ip::tcp::endpoint remote_ep =
            socket_.remote_endpoint(ec);
        if (ec)
            return false;
        return isLegacyAdminPeerAddress(remote_ep.address());
    } catch (...) {
        return false;
    }
}

std::string asio_server::session::getRemoteIP() const {
    if (!remoteIP_.empty())
        return remoteIP_;

    boost::system::error_code ec;
    boost::asio::ip::tcp::endpoint remote_ep = socket_.remote_endpoint(ec);
    if (ec)
        return "<disconnected>";
    boost::asio::ip::address remote_ad = remote_ep.address();

    return remote_ad.to_string();
}

bool asio_server::session::trustCommand(
    Header& requestHeader,
    std::optional<uint_fast32_t>& saveMessageUserId)
{
    try {
        if (requestHeader.command == COMMAND::NEGOTIATE_PROTOCOL_V1)
            return requestHeader.size == 0;

        if (isRequestCommand(requestHeader.command))
            return true;

        if (static_cast<COMMAND>(
                static_cast<uint64_t>(requestHeader.command) >> 32) ==
            COMMAND::SAVE_MESSAGE_TO)
        {
            // LEGACY: user id закодирован в младших 32 битах команды.
            saveMessageUserId = static_cast<uint_fast32_t>(
                static_cast<uint64_t>(requestHeader.command) & 0xFFFFFFFF);
            requestHeader.command = COMMAND::SAVE_MESSAGE_TO;
            return true;
        }

        return false;
    } catch (const std::exception& e) {
        search_server::addToLog(std::string("Exception in trustCommand: ") + e.what());
        return false;
    }
}

void asio_server::Interface::setSearchServer(search_server::SearchServer *_server) {
    searchServer_ = _server;

    cmdMap[COMMAND::SOLOREQUEST] = std::make_unique<SoloRequestCmd>(searchServer_);
    cmdMap[COMMAND::LOAD_TLG_TO_SEND] = std::make_unique<SaveTlgToSendCmd>(L"D:\\");
    cmdMap[COMMAND::LOAD_RAZN] = std::make_unique<SaveFileDefaultCmd>("D:\\OPIS_ADMIN\\РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ\\");
    cmdMap[COMMAND::FILETEXT] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>& v){ return GetFileCmd::downloadFileResultByPath(v);});
    cmdMap[COMMAND::GETBINFILE] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>& v){ return GetFileCmd::downloadFileResultByPath(v);});
    cmdMap[COMMAND::GET_VH_TELEGI_FROM_SQL] = std::make_unique<GetJsonTelegaVhCmd>();
    cmdMap[COMMAND::GET_ISH_TELEGI_FROM_SQL] = std::make_unique<GetJsonTelegaIshCmd>();
    cmdMap[COMMAND::GETSQLJSONANSWEAR] = std::make_unique<GetSqlJsonAnswearCmd>(searchServer_);
    cmdMap[COMMAND::START_UPDATE_BASE] = std::make_unique<StartDictionaryUpdateCmd>(searchServer_);
    cmdMap[COMMAND::USER_REGISTRY] = std::make_unique<RegisterUserCmd>();
    cmdMap[COMMAND::PING] = std::make_unique<PingCmd>();
    // LEGACY: endpoint сохранён только для совместимости старых клиентов.
    cmdMap[COMMAND::GET_MESSAGE] = std::make_unique<GetMessageCmd>();
    cmdMap[COMMAND::GET_ISH_TELEGA_WAY] = std::make_unique<GetTelegaWayIshCmd>();
    cmdMap[COMMAND::GET_VH_TELEGA_WAY] = std::make_unique<GetTelegaWayVhCmd>();
    cmdMap[COMMAND::GET_OPIS_BASE] = std::make_unique<GetFileCmd>([] (const std::vector<uint8_t>&){ return GetFileCmd::downloadFileResultByPath("D:\\OPIS_ADMIN\\" + year_ + ".db"); });
    cmdMap[COMMAND::GET_ATTACHMENTS] = std::make_unique<GetAttachmentsCmd>();
    cmdMap[COMMAND::GET_ISH_PDTV] = std::make_unique<GetIshTelegaPdtvCommand>();
    cmdMap[COMMAND::GET_TELEGA_ATACHMENTS] = std::make_unique<GetTelegaAttachmentsCmd>();
    cmdMap[COMMAND::GET_SINGLE_ATACHMENT] = std::make_unique<GetTelegaSingleAttachmentCmd>();
}

command_execution::CommandResult asio_server::Interface::execCommand(
    asio_server::COMMAND _command,
    std::vector<uint8_t>& _request)
{
    const auto command = cmdMap.find(_command);
    if (command == cmdMap.end() || !command->second) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::CommandNotRegistered,
            getTextCommand(_command));
    }

    try {
        return command->second->executeResult(_request);
    } catch (const std::exception& e) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::CommandExecutionFailed,
            e.what());
    } catch (...) {
        return command_execution::CommandResult::failure(
            command_execution::ErrorCode::CommandExecutionFailed,
            "unknown command exception");
    }
}

void asio_server::Interface::setYear(const std::string &_year) {
    year_ = _year;
}

std::string asio_server::Interface::getYear() {
   return year_;
}

void asio_server::AsioServer::do_accept() {
    if (stopping_.load(std::memory_order_acquire))
        return;

    acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec && !stopping_.load(std::memory_order_acquire)) {
                    if (activity_->tryStartSession()) {
                        bool session_created = false;
                        try {
                            auto new_session = std::make_shared<session>(
                                std::move(socket),
                                cpu_pool_,
                                activity_
                            );
                            session_created = true;
                            {
                                std::lock_guard<std::mutex> lock(sessions_mutex_);
                                std::erase_if(sessions_, [](const auto& item) {
                                    return item.expired();
                                });
                                sessions_.push_back(new_session);
                            }
                            new_session->start();
                        } catch (...) {
                            if (!session_created)
                                activity_->finishSession();
                            search_server::addToLog(
                                "failed to create or start client session"
                            );
                        }
                    }
                } else if (ec != boost::asio::error::operation_aborted &&
                           !stopping_.load(std::memory_order_acquire)) {
                    search_server::addToLog(
                        std::string("accept failed: ") + ec.message()
                    );
                }

                if (!stopping_.load(std::memory_order_acquire))
                    do_accept();
            });
}

void asio_server::session::finishSession() noexcept
{
    if (!session_finished_.exchange(true, std::memory_order_acq_rel) &&
        activity_)
    {
        activity_->finishSession();
    }
}

asio_server::AsioServer::AsioServer(
        boost::asio::io_context& net_io,
        boost::asio::thread_pool& cpu_pool,
        unsigned short port
)
        : net_io_(net_io)
        , cpu_pool_(cpu_pool)
        , acceptor_(net_io_, tcp::endpoint(tcp::v4(), port))
        , activity_(std::make_shared<ServerActivity>())
{
    do_accept();
}

asio_server::AsioServer::~AsioServer()
{
    stop();
}

void asio_server::AsioServer::stop()
{
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;

    activity_->stopAccepting();

    boost::system::error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);

    std::vector<std::shared_ptr<session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions.reserve(sessions_.size());
        for (auto& item : sessions_)
            if (auto active = item.lock())
                sessions.push_back(std::move(active));
        sessions_.clear();
    }
    for (auto& active : sessions)
        active->stop("server shutdown");
}

void asio_server::AsioServer::wait()
{
    activity_->wait();
}

void asio_server::Interface::shutdown()
{
    cmdMap.clear();
    searchServer_ = nullptr;
    year_.clear();
}
