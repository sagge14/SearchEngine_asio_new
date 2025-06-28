#pragma once

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
/** ------------------------FILE_START------------------------ **/
    template<class T>
    class File
    {
        T data;
        size_t hvost{};

    public:

        static const size_t block_size = 64 * 1024;

        auto getBlocksCount() const;
        auto getSizeInBytes() const { return data.size(); };

        std::vector<BYTE> operator[](int col) const;

        File(T _data);
        File(filesystem::path p);
    };

    template<class T>
    File<T>::File(filesystem::path p) {
        std::ifstream file(p, std::ios::binary);
        if(!file.is_open())
            return;
        // Stop eating new lines in binary mode!!!
        file.unsetf(std::ios::skipws);
        // get its size:
        size_t fileSize;
        file.seekg(0, std::ios::end);
        fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        // reserve capacity
        hvost = fileSize % block_size;
        data.reserve(fileSize );
        // read the data:
        data.insert(data.begin(),
                    std::istream_iterator<BYTE>(file),
                    std::istream_iterator<BYTE>());

    }

    template<class T>
    std::vector<BYTE> File<T>::operator[](int col) const {
            size_t start, end;

            if(col == getBlocksCount() - 1 && hvost > 0)
                end = hvost;
            else
                end = block_size;


            start = col * block_size;
            auto first = data.begin() + start ;
            auto last = first + end;

            return std::move(std::vector<BYTE>(first, last));
    }

    template<class T>
    File<T>::File(T _data) {
        std::swap(data, _data);
        hvost = data.size() % block_size;
    }

    template<class T>
    auto File<T>::getBlocksCount() const {
            if(hvost > 0)
                return data.size() / block_size + 1;
            else
                return data.size() / block_size;
    }
/** ------------------------FILE_END------------------------ **/
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


        std::mutex v_data_mutex;
        Header header_{};
        tcp::socket socket_;
        std::mutex socket_mutex;
        unsigned char data_[max_length]{};
        std::vector<BYTE> v_data_{};
        std::vector<BYTE> binFile_{};
        std::string request_;
        uint_fast32_t userId_{};
        std::string userName_ = "default_user";
        std::queue<std::shared_ptr<std::vector<BYTE>>> writeQueue;
        bool isWriting = false;
        std::unique_ptr<std::ifstream> file_stream;

        void doWrite();



        void commandExec();
        void writeHeader();
        bool trustCommand();
        void readHeader();
        void readData();
        std::string getRemoteIP() const;

        void writeToSocket(const std::string& str);

        void sendNextFileChunk();

        template<class T>
        [[maybe_unused]] void writeToSocket(const File<T>& f);
        void writeToSocket(const std::filesystem::path& file_path);

    public:

        void start();
        explicit session(tcp::socket socket) : socket_(std::move(socket)){};
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
