#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio.hpp>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <variant>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include "Commands/Command.h"


namespace search_server
{
    class SearchServer;
}

namespace asio_server
{
    using namespace boost::asio::ip;
    using namespace std;
/** ------------------------COMMAND-------------------------- **/
// Список только имён
// LEGACY: GET_MESSAGE и SAVE_MESSAGE_TO сохранены только ради совместимости
// старого wire-протокола. Для новой функциональности их не использовать.
#define COMMAND_LIST \
    X(SOMEERROR) \
    X(SOLOREQUEST) \
    X(FILETEXT) \
    X(JSONREGUEST) \
    X(ADDRESOLUTION) \
    X(UPDATE) \
    X(GETRESOLUTIONS) \
    X(GETRESOLUTION) \
    X(GETDOCS) \
    X(GETDOC) \
    X(GETSQLJSONANSWEAR) \
    X(GETBINFILE) \
    X(GET_VH_TELEGI_FROM_SQL) \
    X(GET_ISH_TELEGI_FROM_SQL) \
    X(START_UPDATE_BASE) \
    X(LOAD_TLG_TO_SEND) \
    X(GET_MESSAGE) \
    X(USER_REGISTRY) \
    X(PING) \
    X(GET_VH_TELEGA_WAY) \
    X(GET_ISH_TELEGA_WAY) \
    X(GET_OPIS_BASE) \
    X(LOAD_RAZN) \
    X(GET_ATTACHMENTS) \
    X(GET_ISH_PDTV) \
    X(GET_TELEGA_ATACHMENTS) \
    X(GET_SINGLE_ATACHMENT)   \
    X(SERVER_BUSY_ERROR)   \
    X(END_COMMAND)   \

// enum
    enum class COMMAND : uint_fast64_t {
        #define X(name) name,
                COMMAND_LIST
        #undef X
        // Explicit extension values: never insert them into COMMAND_LIST,
        // otherwise the established values 0..28 would move.
        ERROR_RESPONSE = 29,
        NEGOTIATE_PROTOCOL_V1 = 30,
        AUTHENTICATE_V1 = 31,
        GET_TELEGA_TEXT = 32,
        // LEGACY: специальное составное wire-значение, не расширять.
        SAVE_MESSAGE_TO = 2781032419
    };

// функция enum → string
    inline const char* to_string(COMMAND cmd) {
        switch (cmd) {
        #define X(name) case COMMAND::name: return #name;
                    COMMAND_LIST
        #undef X
                    case COMMAND::ERROR_RESPONSE: return "ERROR_RESPONSE";
                    case COMMAND::NEGOTIATE_PROTOCOL_V1: return "NEGOTIATE_PROTOCOL_V1";
                    case COMMAND::AUTHENTICATE_V1: return "AUTHENTICATE_V1";
                    case COMMAND::GET_TELEGA_TEXT: return "GET_TELEGA_TEXT";
                    default:
                return "UNKNOWN COMMAND";
        }
    }

    inline std::string getTextCommand(COMMAND command) {
        return to_string(command);
    }


    struct Header
    {
        uint_fast64_t size{};
        COMMAND command{};
    };

    namespace search_protocol
    {
        inline constexpr std::uint32_t ERROR_RESPONSE_VERSION = 1;
        inline constexpr std::uint32_t PROTOCOL_CAPABILITIES_VERSION = 1;
        inline constexpr std::uint32_t CAPABILITY_TYPED_ERRORS_V1 = 1u << 0;
        inline constexpr std::uint32_t CAPABILITY_CLIENT_AUTH_V1 = 1u << 1;

        struct ErrorResponseV1
        {
            std::uint32_t version{ERROR_RESPONSE_VERSION};
            std::uint32_t errorCode{};
        };

        struct ProtocolCapabilitiesV1
        {
            std::uint32_t version{PROTOCOL_CAPABILITIES_VERSION};
            std::uint32_t capabilities{CAPABILITY_TYPED_ERRORS_V1};
        };
    }

    [[nodiscard]] inline constexpr bool isRequestCommand(COMMAND command) noexcept
    {
        switch (command)
        {
            case COMMAND::SOLOREQUEST:
            case COMMAND::FILETEXT:
            case COMMAND::GETSQLJSONANSWEAR:
            case COMMAND::GETBINFILE:
            case COMMAND::GET_VH_TELEGI_FROM_SQL:
            case COMMAND::GET_ISH_TELEGI_FROM_SQL:
            case COMMAND::START_UPDATE_BASE:
            case COMMAND::LOAD_TLG_TO_SEND:
            case COMMAND::GET_MESSAGE: // LEGACY
            case COMMAND::USER_REGISTRY:
            case COMMAND::PING:
            case COMMAND::GET_VH_TELEGA_WAY:
            case COMMAND::GET_ISH_TELEGA_WAY:
            case COMMAND::GET_OPIS_BASE:
            case COMMAND::LOAD_RAZN:
            case COMMAND::GET_ATTACHMENTS:
            case COMMAND::GET_ISH_PDTV:
            case COMMAND::GET_TELEGA_ATACHMENTS:
            case COMMAND::GET_SINGLE_ATACHMENT:
            case COMMAND::GET_TELEGA_TEXT:
            case COMMAND::NEGOTIATE_PROTOCOL_V1:
            case COMMAND::AUTHENTICATE_V1:
                return true;
            default:
                return false;
        }
    }

    /// Commands allowed before session authorization (USER_REGISTRY or AUTHENTICATE_V1).
    [[nodiscard]] inline constexpr bool isSessionBootstrapCommand(
        COMMAND command) noexcept
    {
        switch (command)
        {
            case COMMAND::NEGOTIATE_PROTOCOL_V1:
            case COMMAND::USER_REGISTRY:
            case COMMAND::AUTHENTICATE_V1:
            case COMMAND::PING:
                return true;
            default:
                return false;
        }
    }

    /// Legacy USER_REGISTRY authorizes only the exact admin session name.
    /// Any other payload must fail closed (AuthFailed + TCP close).
    [[nodiscard]] inline bool isLegacyAdminUserRegistryPayload(
        std::string_view payload) noexcept
    {
        return payload == "admin";
    }

    /// Strict IPv4 localhost peer check for legacy admin authorization.
    /// Only 127.0.0.1 is accepted — not ::1, not other 127.0.0.0/8 addresses.
    [[nodiscard]] inline bool isLegacyAdminPeerAddress(
        const boost::asio::ip::address& remote_peer) noexcept
    {
        return remote_peer.is_v4()
            && remote_peer.to_v4() == boost::asio::ip::address_v4::loopback();
    }

    /// Combined legacy-admin gate used by session and regression tests.
    /// peerLookupSucceeded must be true only when remote_endpoint() succeeded;
    /// lookup failure fails closed (never authorizes admin).
    [[nodiscard]] inline bool mayAuthorizeLegacyAdmin(
        std::string_view payload,
        bool peerLookupSucceeded,
        const boost::asio::ip::address& remote_peer) noexcept
    {
        return isLegacyAdminUserRegistryPayload(payload)
            && peerLookupSucceeded
            && isLegacyAdminPeerAddress(remote_peer);
    }

    /// Session wire gate used by commandExec before any data handler runs.
    struct SessionCommandGateDecision
    {
        bool allow_execute{false};
        /// When allow_execute is false, AuthRequired must close the TCP session.
        bool close_after_auth_required{true};
    };

    [[nodiscard]] inline constexpr SessionCommandGateDecision
    evaluateSessionCommandGate(
        COMMAND command,
        bool authenticated) noexcept
    {
        if (isSessionBootstrapCommand(command) || authenticated)
            return {true, true};
        return {false, true};
    }

    [[nodiscard]] inline constexpr COMMAND legacyErrorCommand(
        command_execution::ErrorCode error) noexcept
    {
        return error == command_execution::ErrorCode::ServerBusy
            ? COMMAND::SERVER_BUSY_ERROR
            : COMMAND::SOMEERROR;
    }

    [[nodiscard]] inline constexpr Header makeLegacyErrorHeader(
        command_execution::ErrorCode error) noexcept
    {
        return Header{0, legacyErrorCommand(error)};
    }

    [[nodiscard]] inline constexpr search_protocol::ErrorResponseV1
    makeTypedErrorResponse(command_execution::ErrorCode error) noexcept
    {
        return search_protocol::ErrorResponseV1{
            search_protocol::ERROR_RESPONSE_VERSION,
            static_cast<std::uint32_t>(error)};
    }

    static_assert(sizeof(uint_fast64_t) == 8);
    static_assert(sizeof(Header) == 16);
    static_assert(std::is_trivially_copyable_v<Header>);
    static_assert(static_cast<uint_fast64_t>(COMMAND::SOMEERROR) == 0);
    static_assert(static_cast<uint_fast64_t>(COMMAND::SOLOREQUEST) == 1);
    static_assert(static_cast<uint_fast64_t>(COMMAND::FILETEXT) == 2);
    static_assert(static_cast<uint_fast64_t>(COMMAND::GETBINFILE) == 11);
    static_assert(static_cast<uint_fast64_t>(COMMAND::PING) == 18);
    static_assert(static_cast<uint_fast64_t>(COMMAND::GET_SINGLE_ATACHMENT) == 26);
    static_assert(static_cast<uint_fast64_t>(COMMAND::SERVER_BUSY_ERROR) == 27);
    static_assert(static_cast<uint_fast64_t>(COMMAND::END_COMMAND) == 28);
    static_assert(static_cast<uint_fast64_t>(COMMAND::ERROR_RESPONSE) == 29);
    static_assert(static_cast<uint_fast64_t>(COMMAND::NEGOTIATE_PROTOCOL_V1) == 30);
    static_assert(static_cast<uint_fast64_t>(COMMAND::AUTHENTICATE_V1) == 31);
    static_assert(static_cast<uint_fast64_t>(COMMAND::GET_TELEGA_TEXT) == 32);
    static_assert(static_cast<uint_fast64_t>(COMMAND::SAVE_MESSAGE_TO) == 2781032419ULL);
    static_assert(sizeof(search_protocol::ErrorResponseV1) == 8);
    static_assert(sizeof(search_protocol::ProtocolCapabilitiesV1) == 8);
    static_assert(std::is_trivially_copyable_v<search_protocol::ErrorResponseV1>);
    static_assert(std::is_trivially_copyable_v<search_protocol::ProtocolCapabilitiesV1>);
/** ------------------------session_START------------------------ **/
    class Interface;

    struct ServerActivity {
        bool tryStartCommand();
        void finishCommand() noexcept;
        bool tryStartSession();
        void finishSession() noexcept;
        void stopAccepting() noexcept;
        void wait();

        std::mutex mutex;
        std::condition_variable condition;
        std::size_t active_commands{0};
        std::size_t active_sessions{0};
        bool stopping{false};
    };

    class session : public std::enable_shared_from_this<session>
    {
        /// Буфер ответов: 1 = минимум (тест), 4–8 = меньше блокировок commandExec и надёжнее доставка try_send(ошибок)
        static constexpr std::size_t kWriteChannelCapacity = 8;

        enum { max_length = 64 * 1024 };

        struct ResponseWrite
        {
            asio_server::Header header;
            std::shared_ptr<std::vector<BYTE>> payload;
        };

        struct FileTransfer
        {
            asio_server::Header header;
            std::shared_ptr<std::ifstream> stream;
        };

        struct ErrorWrite
        {
            asio_server::Header header;
            std::shared_ptr<std::vector<BYTE>> payload;
            bool closeAfterWrite{};
        };

        using WriteItem = std::variant<ResponseWrite, FileTransfer, ErrorWrite>;

        tcp::socket socket_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        boost::asio::thread_pool& cpu_pool_;
        std::string userName_ = "default_user";
        std::string clientId_;
        std::string deviceType_;
        std::string deviceId_;
        /// Session gate: set only by USER_REGISTRY("admin")+127.0.0.1 peer
        /// or successful AUTHENTICATE_V1 (any peer).
        bool authenticated_{false};
        std::string remoteIP_;
        mutable std::mutex user_name_mutex_;
        std::atomic_bool stopped_{false};
        std::atomic<bool> session_finished_{false};
        std::atomic<bool> terminal_error_queued_{false};
        std::atomic_bool typed_errors_enabled_{false};
        std::shared_ptr<ServerActivity> activity_;


        boost::asio::experimental::concurrent_channel<
            void(boost::system::error_code, WriteItem)> write_channel_;

        boost::asio::awaitable<void> commandExec(
            Header requestHeader,
            std::vector<BYTE> requestData,
            std::optional<uint_fast32_t> saveMessageUserId);
        bool trustCommand(
            Header& requestHeader,
            std::optional<uint_fast32_t>& saveMessageUserId);
        boost::asio::awaitable<bool> queueError(
            command_execution::ErrorCode error,
            std::string diagnostic = {},
            bool closeAfterWrite = false,
            COMMAND requestCommand = COMMAND::SOMEERROR);
        /// True only when TCP remote peer is exactly 127.0.0.1 (fail closed).
        [[nodiscard]] bool isLocalAdminPeer() const;
        std::string getRemoteIP() const;
        void stopOnExecutor(const std::string& why);
        void finishSession() noexcept;

        boost::asio::awaitable<void> readLoop();
        boost::asio::awaitable<void> writeLoop();
        boost::asio::awaitable<bool> sendFile(
            const std::shared_ptr<std::ifstream>& stream,
            uint_fast64_t expectedSize);

    public:

        void start();
        void stop(const char* why);
        explicit session(tcp::socket socket,
                         boost::asio::thread_pool& cpu_pool,
                         std::shared_ptr<ServerActivity> activity)
            :socket_(std::move(socket))
            ,strand_(boost::asio::make_strand(socket_.get_executor()))
            ,cpu_pool_(cpu_pool)
            ,activity_(std::move(activity))
            ,write_channel_(strand_, kWriteChannelCapacity){};
       ~session();

    };

    class AsioServer
    {
        public:
            AsioServer(
                    boost::asio::io_context& net_io,
                    boost::asio::thread_pool& cpu_pool,
                    unsigned short port
            );
            ~AsioServer();

            void stop();
            void wait();

        private:

            void do_accept();

            boost::asio::io_context& net_io_;
            boost::asio::thread_pool& cpu_pool_;
            tcp::acceptor acceptor_;
            std::atomic<bool> stopping_{false};
            std::shared_ptr<ServerActivity> activity_;
            std::mutex sessions_mutex_;
            std::vector<std::weak_ptr<session>> sessions_;
    };
/** ------------------------session_END------------------------ **/

/** ------------------------Interface_START------------------------ **/
    struct ProductionCommandPaths
    {
        std::string tlg_send_root;
        std::string razn_output_dir;
        std::string opis_base_dir;
        std::filesystem::path attachmentsConfigPath;
    };

    class Interface
    {
        inline static std::string year_ = {};
        inline static std::string opis_base_dir_ = {};
        inline static search_server::SearchServer* searchServer_ = nullptr;
        inline static std::map<COMMAND, std::unique_ptr<Command>> cmdMap{};

    public:
        static void setYear(const std::string& year);
        static std::string getYear();
        static void setSearchServer(
            search_server::SearchServer* _server,
            const ProductionCommandPaths& paths);
        static void shutdown();
        [[nodiscard]] static command_execution::CommandResult execCommand(
            COMMAND _command,
            std::vector<uint8_t>& _request);
    };

}
/** ------------------------Interface_END------------------------ **/
