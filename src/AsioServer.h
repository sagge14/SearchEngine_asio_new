#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio.hpp>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <variant>
#include <map>
#include <queue>
#include <Commands/Command.h>


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
    X(GET_SINGLE_ATACHMENT) \
    X(END_COMMAND) \

// enum
    enum class COMMAND : uint_fast64_t {
        #define X(name) name,
                COMMAND_LIST
        #undef X
        SAVE_MESSAGE_TO = 2781032419
    };

// функция enum → string
    inline const char* to_string(COMMAND cmd) {
        switch (cmd) {
        #define X(name) case COMMAND::name: return #name;
                    COMMAND_LIST
        #undef X
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
/** ------------------------session_START------------------------ **/
    class Interface;

    class session : public std::enable_shared_from_this<session>
    {

        enum { max_length = 64 * 1024 };

        using WriteItem = std::variant<asio_server::Header, std::shared_ptr<std::vector<BYTE>>,
        std::filesystem::path>;

        tcp::socket socket_;
        Header header_{};

        std::vector<BYTE> v_data_{};
        std::string request_;
        uint_fast32_t userId_{};
        std::string userName_ = "default_user";
        std::atomic_bool stopped_{false};


        std::unique_ptr<std::ifstream> file_stream;

        boost::asio::experimental::channel<void(boost::system::error_code, WriteItem)> write_channel_;

        void commandExec();
        bool trustCommand();
        std::string getRemoteIP() const;
        void stop(const char* why);

        template<typename T>
        bool safe_send_to_channel(T&& item);

        template<typename... Args>
        bool safe_send_to_channel(Args&&... args) {
            return (... && safe_send_to_channel(std::forward<Args>(args)));
        }

        boost::asio::awaitable<void> readLoop();
        boost::asio::awaitable<void> writeLoop();
        boost::asio::awaitable<void> sendNextFileChunk();
        boost::asio::awaitable<bool> openFileStream(const std::filesystem::path& file_path);

    public:

        void start();
        explicit session(tcp::socket socket) : socket_(std::move(socket)), write_channel_(socket_.get_executor(), 2000){};
       ~session();

    };

    class AsioServer
    {
    public:
        AsioServer(boost::asio::io_context& io_context, short port);

    private:
        void do_accept();
        tcp::acceptor acceptor_;
    };
/** ------------------------session_END------------------------ **/

/** ------------------------Interface_START------------------------ **/
    class Interface
    {
        inline static std::string year_ = {};
        inline static search_server::SearchServer* searchServer_ = nullptr;
        inline static std::map<COMMAND, std::unique_ptr<Command>> cmdMap{};

    public:
        static void setYear(const std::string& year);
        static std::string getYear();
        static void setSearchServer(search_server::SearchServer* _server);
        static void setCommand(COMMAND command, std::unique_ptr<Command> myCmd);
        static std::vector<uint8_t> execCommand(COMMAND _command, std::vector<uint8_t>& _request);
    };

}
/** ------------------------Interface_END------------------------ **/
