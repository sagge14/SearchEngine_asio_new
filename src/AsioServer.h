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
    enum class COMMAND : uint_fast64_t
    {
        SOMEERROR = 0,
        SOLOREQUEST,
        FILETEXT,
        JSONREGUEST,
        ADDRESOLUTION,
        UPDATE,
        GETRESOLUTIONS,
        GETRESOLUTION,
        GETDOCS,
        GETDOC,
        GETSQLJSONANSWEAR,
        GETBINFILE,
        GET_VH_TELEGI_FROM_SQL,
        GET_ISH_TELEGI_FROM_SQL,
        START_UPDATE_BASE,
        LOAD_TLG_TO_SEND,
        GET_MESSAGE,
        USER_REGISTRY,
        PING,
        GET_VH_TELEGA_WAY,
        GET_ISH_TELEGA_WAY,
        GET_OPIS_BASE,
        LOAD_RAZN,
        GET_ATTACHMENTS,
        END_COMMAND,
        //--------------------
        SAVE_MESSAGE_TO = 2781032419,
        //--------------------
    };

    std::string getTextCommand(COMMAND command);

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


       // std::mutex v_data_mutex;
        Header header_{};
        tcp::socket socket_;
        //std::mutex socket_mutex;
        std::vector<BYTE> v_data_{};
       // std::vector<BYTE> binFile_{};
        std::string request_;
        uint_fast32_t userId_{};
        std::string userName_ = "default_user";
        boost::asio::experimental::channel<void(boost::system::error_code, WriteItem)> write_channel_;
        std::unique_ptr<std::ifstream> file_stream;

        void commandExec();
        bool trustCommand();
        std::string getRemoteIP() const;

        boost::asio::awaitable<void> readLoop();
        boost::asio::awaitable<void> writeLoop();
        boost::asio::awaitable<void> sendNextFileChunk();
        boost::asio::awaitable<bool> openFileStream(const std::filesystem::path& file_path);

    public:

        void start();
        explicit session(tcp::socket socket) : socket_(std::move(socket)), write_channel_(socket_.get_executor(), 100000){};
       ~session();

    };

    class AsioServer
    {
    public:
        AsioServer(boost::asio::io_context& io_context, short port);
        //boost::asio::io_context* io_context_;


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
