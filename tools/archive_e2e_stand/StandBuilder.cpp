#include "StandBuilder.h"

#include "Backup/FileHash.h"
#include "MyUtils/Encoding.h"
#include "nlohmann/json.hpp"
#include "sqlite3.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace searchengine_archive_e2e {
namespace {

using json = nlohmann::json;
using Column = std::pair<const char*, const char*>;

constexpr char kMarkerName[] = ".searchengine-archive-e2e-stand";
constexpr char kManifestName[] = "stand-manifest.json";
constexpr char kArchiveManifestName[] = "archive-operation.json";
constexpr int kF12WaySchemaVersion = 4;

const std::vector<Column>& prmColumns()
{
    static const std::vector<Column> columns{
        {"Index", "INTEGER NOT NULL"}, {"TelNo", "INTEGER"},
        {"Sr", "VARCHAR"}, {"FFrom", "VARCHAR"},
        {"Familia", "VARCHAR"}, {"TTime", "VARCHAR"},
        {"DData", "VARCHAR"}, {"Data", "VARCHAR"},
        {"PodpNo", "VARCHAR"}, {"DataPodp", "VARCHAR"},
        {"Sekretno", "VARCHAR"}, {"Ekzempl", "VARCHAR"},
        {"Lists", "VARCHAR"}, {"Dej", "VARCHAR"},
        {"Podrazd", "VARCHAR"}, {"Adresat", "VARCHAR"},
        {"NoEkz", "VARCHAR"}, {"NoFN", "VARCHAR"},
        {"EK", "VARCHAR"}, {"KolPril", "VARCHAR"},
        {"Psekretno", "VARCHAR"}, {"SizeAll", "VARCHAR"},
        {"FileName", "VARCHAR"}, {"SetevNo", "VARCHAR"},
        {"NoSetev", "VARCHAR"}, {"Keys", "VARCHAR"},
        {"Primechanie", "VARCHAR"}, {"Grups", "VARCHAR"},
        {"PDTV", "VARCHAR"}, {"GdeSHT", "VARCHAR"},
        {"DirectTo", "VARCHAR"}, {"PrilName", "VARCHAR"},
        {"NamePril", "VARCHAR"}, {"Copyes", "VARCHAR"},
        {"Edit", "VARCHAR"}, {"CHM", "VARCHAR"},
        {"DeleteCHM", "VARCHAR"}, {"Edit1", "VARCHAR"},
        {"Edit2", "VARCHAR"}, {"PrilName1", "VARCHAR"},
        {"PrilName2", "VARCHAR"}, {"SizeAll1", "VARCHAR"},
        {"SizeAll2", "VARCHAR"}, {"Copyes1", "VARCHAR"},
        {"Copyes2", "VARCHAR"}, {"DataK", "VARCHAR"},
        {"KPODI", "VARCHAR"}, {"ISTOK", "VARCHAR"}};
    return columns;
}

const std::vector<Column>& prdColumns()
{
    static const std::vector<Column> columns{
        {"Index", "INT NOT NULL"}, {"DData", "VARCHAR(10)"},
        {"Lists", "VARCHAR(5)"}, {"Lichno", "VARCHAR(15)"},
        {"PrilName", "VARCHAR(8000)"}, {"Sekretno", "VARCHAR(6)"},
        {"PodpNo", "VARCHAR(35)"}, {"Familia", "VARCHAR(35)"},
        {"FFrom", "VARCHAR(8000)"}, {"FFrom1", "VARCHAR(8000)"},
        {"FFrom2", "VARCHAR(8000)"}, {"FFrom3", "VARCHAR(8000)"},
        {"FFrom4", "VARCHAR(8000)"}, {"FFrom5", "VARCHAR(8000)"},
        {"FFrom6", "VARCHAR(8000)"}, {"FFrom7", "VARCHAR(8000)"},
        {"FFrom8", "VARCHAR(8000)"}, {"FFrom9", "VARCHAR(8000)"},
        {"FFrom10", "VARCHAR(8000)"}, {"SetevNo", "VARCHAR(8000)"},
        {"Keys", "VARCHAR(35)"}, {"Grups", "VARCHAR(5)"},
        {"TTime", "VARCHAR(5)"}, {"PTime", "VARCHAR(5)"},
        {"Dej", "VARCHAR(35)"}, {"CHM", "VARCHAR(10)"},
        {"DeleteCHM", "VARCHAR(15)"}, {"GdeSHT", "VARCHAR(8000)"},
        {"Primechanie", "VARCHAR(35)"}, {"TelNo", "VARCHAR(20)"},
        {"Sr", "VARCHAR(5)"}, {"DataPodp", "VARCHAR(10)"},
        {"Ekzempl", "VARCHAR(5)"}, {"Podrazd", "VARCHAR(8000)"},
        {"EK", "VARCHAR(5)"}, {"KolPril", "VARCHAR(5)"},
        {"Psekretno", "VARCHAR(8000)"}, {"SizeAll", "VARCHAR(8000)"},
        {"FileName", "VARCHAR(35)"}, {"NoSetev", "VARCHAR(10)"},
        {"PDTV", "VARCHAR(35)"}, {"DirectTo", "VARCHAR(8000)"},
        {"Copyes", "VARCHAR(8000)"}, {"Blank", "VARCHAR(8000)"},
        {"AllPDTV1", "VARCHAR(8000)"}, {"AllPDTV2", "VARCHAR(8000)"},
        {"AllPDTV3", "VARCHAR(8000)"}, {"Edit", "VARCHAR(8000)"},
        {"Edit1", "VARCHAR(8000)"}, {"Edit2", "VARCHAR(8000)"},
        {"PrilName1", "VARCHAR(8000)"}, {"PrilName2", "VARCHAR(8000)"},
        {"SizeAll1", "VARCHAR(8000)"}, {"SizeAll2", "VARCHAR(8000)"},
        {"SetevNo1", "VARCHAR(8000)"}, {"AllPDTV4", "VARCHAR(8000)"},
        {"AllPDTV5", "VARCHAR(8000)"}, {"AllPDTV6", "VARCHAR(8000)"},
        {"AllPDTV7", "VARCHAR(8000)"}, {"AllPDTV8", "VARCHAR(8000)"},
        {"AllPDTV9", "VARCHAR(8000)"}, {"AllPDTV10", "VARCHAR(8000)"},
        {"FFrom11", "VARCHAR(8000)"}, {"FFrom12", "VARCHAR(8000)"},
        {"FFrom13", "VARCHAR(8000)"}, {"FFrom14", "VARCHAR(8000)"},
        {"FFrom15", "VARCHAR(8000)"}, {"Adresat", "VARCHAR(10)"},
        {"NoEkz", "VARCHAR(5)"}, {"NoFN", "VARCHAR(5)"},
        {"Data", "VARCHAR(10)"}, {"Copyes1", "VARCHAR(8000)"},
        {"Copyes2", "VARCHAR(8000)"}, {"GdeSHT1", "VARCHAR(8000)"},
        {"Control", "VARCHAR(10)"}};
    return columns;
}

std::string utf8(const fs::path& path)
{
    return encoding::wstring_to_utf8(path.wstring());
}

fs::path fromUtf8(const std::string& value)
{
    return fs::path(encoding::utf8_to_wstring(value));
}

fs::path absoluteNormalized(const fs::path& path)
{
    std::error_code error;
    fs::path result = fs::absolute(path, error);
    if (error)
        throw std::runtime_error("cannot make path absolute: " + error.message());
    return result.lexically_normal();
}

bool isDriveRoot(const fs::path& path)
{
    const fs::path normalized = path.lexically_normal();
    return !normalized.root_path().empty() && normalized == normalized.root_path();
}

struct WorkstationDirectoryInspection {
    bool exists{};
    std::string problem;
};

WorkstationDirectoryInspection inspectWorkstationDirectory(
    const fs::path& path,
    bool requireEmpty)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return {
            false,
            "cannot inspect destination; Win32 error=" +
                std::to_string(error)};
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {true, "destination is a reparse point"};
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return {true, "destination is not a directory"};
    if (!requireEmpty)
        return {true, {}};

    std::error_code error;
    const bool empty = fs::is_empty(path, error);
    if (error) {
        return {
            true,
            "cannot inspect destination directory: " + error.message()};
    }
    if (!empty)
        return {true, "destination directory is not empty"};
    return {true, {}};
}

bool isContained(const fs::path& child, const fs::path& root)
{
    const fs::path normalizedChild = child.lexically_normal();
    const fs::path normalizedRoot = root.lexically_normal();
    const fs::path relative = normalizedChild.lexically_relative(normalizedRoot);
    if (relative.empty())
        return normalizedChild == normalizedRoot;
    for (const auto& component : relative) {
        if (component == L"..")
            return false;
    }
    return !relative.is_absolute();
}

std::string join(const std::vector<std::string>& values, char separator)
{
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0)
            stream << separator;
        stream << values[index];
    }
    return stream.str();
}

std::string joinTerminated(
    const std::vector<std::string>& values,
    char separator)
{
    if (values.empty())
        return {};
    return join(values, separator) + separator;
}

std::vector<std::string> split(const std::string& value, char separator)
{
    std::vector<std::string> result;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, separator)) {
        if (!item.empty())
            result.push_back(item);
    }
    return result;
}

std::vector<std::string> splitTerminatedF12List(const std::string& value)
{
    if (value.empty())
        return {};
    if (value.back() != ';')
        throw std::runtime_error("synthetic F12 list has no trailing delimiter");

    std::vector<std::string> result;
    std::size_t begin = 0;
    const std::size_t contentSize = value.size() - 1;
    while (begin < contentSize) {
        const std::size_t end = value.find(';', begin);
        if (end == std::string::npos || end == begin || end > contentSize)
            throw std::runtime_error("synthetic F12 list contains an empty item");
        result.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

void writeText(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot create file: " + utf8(path));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output.good())
        throw std::runtime_error("cannot write file: " + utf8(path));
}

std::vector<std::uint8_t> payload(std::size_t size, std::uint32_t seed)
{
    std::vector<std::uint8_t> result(size);
    for (std::size_t index = 0; index < size; ++index)
        result[index] = static_cast<std::uint8_t>((seed + index * 37U) & 0xffU);
    return result;
}

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot create file: " + utf8(path));
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    if (!output.good())
        throw std::runtime_error("cannot write file: " + utf8(path));
}

void copyDirectoryTree(const fs::path& source, const fs::path& destination)
{
    if (!fs::is_directory(source))
        throw std::runtime_error("program template is not a directory: " + utf8(source));
    fs::create_directories(destination);
    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
        const fs::path relative = it->path().lexically_relative(source);
        const fs::path target = destination / relative;
        if (it->is_symlink())
            throw std::runtime_error("program template contains a link: " + utf8(it->path()));
        if (it->is_directory()) {
            fs::create_directories(target);
        } else if (it->is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(it->path(), target, fs::copy_options::none);
        } else {
            throw std::runtime_error(
                "program template contains an unsupported entry: " + utf8(it->path()));
        }
    }
}

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void put16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    put16(output, static_cast<std::uint16_t>(value));
    put16(output, static_cast<std::uint16_t>(value >> 16U));
}

std::uint16_t get16(const std::vector<std::uint8_t>& input, std::size_t offset)
{
    if (offset + 2 > input.size())
        throw std::runtime_error("truncated ZIP field");
    return static_cast<std::uint16_t>(input[offset]) |
        static_cast<std::uint16_t>(input[offset + 1] << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& input, std::size_t offset)
{
    return static_cast<std::uint32_t>(get16(input, offset)) |
        (static_cast<std::uint32_t>(get16(input, offset + 2)) << 16U);
}

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> data;
};

void writeStoredZip(const fs::path& path, const std::vector<ZipEntry>& entries)
{
    struct CentralEntry {
        std::string name;
        std::uint32_t crc{};
        std::uint32_t size{};
        std::uint32_t offset{};
    };
    std::vector<std::uint8_t> bytes;
    std::vector<CentralEntry> central;
    for (const auto& entry : entries) {
        if (entry.name.size() > 0xffffU || entry.data.size() > 0xffffffffU)
            throw std::runtime_error("synthetic ZIP entry is too large");
        CentralEntry item;
        item.name = entry.name;
        item.crc = crc32(entry.data);
        item.size = static_cast<std::uint32_t>(entry.data.size());
        item.offset = static_cast<std::uint32_t>(bytes.size());
        put32(bytes, 0x04034b50U);
        put16(bytes, 20); put16(bytes, 0); put16(bytes, 0);
        put16(bytes, 0); put16(bytes, 0);
        put32(bytes, item.crc); put32(bytes, item.size); put32(bytes, item.size);
        put16(bytes, static_cast<std::uint16_t>(entry.name.size()));
        put16(bytes, 0);
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
        bytes.insert(bytes.end(), entry.data.begin(), entry.data.end());
        central.push_back(std::move(item));
    }
    const std::uint32_t centralOffset = static_cast<std::uint32_t>(bytes.size());
    for (const auto& entry : central) {
        put32(bytes, 0x02014b50U);
        put16(bytes, 20); put16(bytes, 20); put16(bytes, 0); put16(bytes, 0);
        put16(bytes, 0); put16(bytes, 0);
        put32(bytes, entry.crc); put32(bytes, entry.size); put32(bytes, entry.size);
        put16(bytes, static_cast<std::uint16_t>(entry.name.size()));
        put16(bytes, 0); put16(bytes, 0); put16(bytes, 0); put16(bytes, 0);
        put32(bytes, 0); put32(bytes, entry.offset);
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
    }
    const std::uint32_t centralSize =
        static_cast<std::uint32_t>(bytes.size()) - centralOffset;
    put32(bytes, 0x06054b50U);
    put16(bytes, 0); put16(bytes, 0);
    put16(bytes, static_cast<std::uint16_t>(central.size()));
    put16(bytes, static_cast<std::uint16_t>(central.size()));
    put32(bytes, centralSize); put32(bytes, centralOffset); put16(bytes, 0);
    writeBytes(path, bytes);
}

std::vector<std::pair<std::string, std::uint32_t>> readStoredZipEntries(
    const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open ZIP: " + utf8(path));
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::vector<std::pair<std::string, std::uint32_t>> result;
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size() && get32(bytes, offset) == 0x04034b50U) {
        if (offset + 30 > bytes.size())
            throw std::runtime_error("truncated ZIP local header");
        if (get16(bytes, offset + 8) != 0)
            throw std::runtime_error("synthetic ZIP entry is compressed");
        const std::uint32_t packed = get32(bytes, offset + 18);
        const std::uint32_t unpacked = get32(bytes, offset + 22);
        const std::uint16_t nameLength = get16(bytes, offset + 26);
        const std::uint16_t extraLength = get16(bytes, offset + 28);
        const std::size_t nameOffset = offset + 30;
        const std::size_t dataOffset = nameOffset + nameLength + extraLength;
        if (packed != unpacked || dataOffset + packed > bytes.size())
            throw std::runtime_error("invalid synthetic ZIP entry size");
        const std::string name(
            bytes.begin() + static_cast<std::ptrdiff_t>(nameOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(nameOffset + nameLength));
        std::vector<std::uint8_t> data(
            bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset + packed));
        if (crc32(data) != get32(bytes, offset + 14))
            throw std::runtime_error("synthetic ZIP CRC mismatch");
        result.emplace_back(name, unpacked);
        offset = dataOffset + packed;
    }
    if (result.empty() || offset + 4 > bytes.size() ||
        get32(bytes, offset) != 0x02014b50U)
    {
        throw std::runtime_error("synthetic ZIP has no central directory");
    }
    return result;
}

class Database final {
public:
    Database(const fs::path& path, int flags)
    {
        const int rc = sqlite3_open_v2(utf8(path).c_str(), &database_, flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string detail = database_ ? sqlite3_errmsg(database_) : "open failed";
            if (database_)
                sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error("SQLite open failed: " + detail);
        }
    }

    ~Database() { if (database_) sqlite3_close(database_); }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* get() const noexcept { return database_; }

    void execute(const std::string& sql)
    {
        char* rawError = nullptr;
        const int rc = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &rawError);
        const std::string detail = rawError ? rawError : sqlite3_errmsg(database_);
        sqlite3_free(rawError);
        if (rc != SQLITE_OK)
            throw std::runtime_error("SQLite execute failed: " + detail);
    }

private:
    sqlite3* database_{};
};

std::string createArchiveSql(const std::vector<Column>& columns)
{
    std::ostringstream sql;
    sql << "CREATE TABLE ARCHIVE (";
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0)
            sql << ',';
        sql << '\"' << columns[index].first << "\" " << columns[index].second;
    }
    sql << ", PRIMARY KEY (\"Index\")) WITHOUT ROWID";
    return sql.str();
}

struct Attachment {
    std::string name;
    std::uint32_t size{};
    std::string security;
};

struct Row {
    int id{};
    bool prm{};
    int month{};
    int record{};
    std::string date;
    std::string directTo;
    std::string fileName;
    std::string mainText;
    std::vector<Attachment> attachments;
};

json readJson(const fs::path& path);
fs::path physicalPath(
    const fs::path& logicalPath,
    const fs::path& logicalRoot,
    const fs::path& physicalRoot);

struct ContentLayout {
    std::array<fs::path, 12> prmMonthNames;
    fs::path tlgName;
};

std::wstring lower(const std::wstring& value)
{
    std::wstring result = value;
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

fs::path configuredLeaf(const json& value)
{
    if (!value.is_string())
        throw std::runtime_error("config.index_roots contains a non-string");
    const fs::path configured = fromUtf8(value.get<std::string>());
    const fs::path leaf = configured.filename();
    if (leaf.empty() || leaf == L"." || leaf == L".." ||
        leaf.has_parent_path())
    {
        throw std::runtime_error(
            "configured index root has no safe directory name: " +
            utf8(configured));
    }
    return leaf;
}

ContentLayout contentLayoutFromSettings(const fs::path& settingsTemplate)
{
    const json settings = readJson(settingsTemplate);
    if (!settings.contains("config") || !settings.at("config").is_object() ||
        !settings.at("config").contains("index_roots") ||
        !settings.at("config").at("index_roots").is_array())
    {
        throw std::runtime_error(
            "Settings template has no config.index_roots array");
    }

    std::vector<fs::path> monthNames;
    ContentLayout layout;
    std::set<std::wstring> uniqueNames;
    for (const auto& value : settings.at("config").at("index_roots")) {
        const fs::path leaf = configuredLeaf(value);
        const std::wstring key = lower(leaf.wstring());
        if (!uniqueNames.insert(key).second) {
            throw std::runtime_error(
                "duplicate configured index-root directory name: " +
                utf8(leaf));
        }
        if (key == L"tlg") {
            if (!layout.tlgName.empty())
                throw std::runtime_error("Settings contains multiple TLG roots");
            layout.tlgName = leaf;
        } else {
            monthNames.push_back(leaf);
        }
    }
    if (layout.tlgName.empty() || monthNames.size() != 12) {
        throw std::runtime_error(
            "Settings template must contain 12 monthly index roots and one TLG root");
    }
    std::copy(monthNames.begin(), monthNames.end(), layout.prmMonthNames.begin());
    return layout;
}

std::string monthText(int month)
{
    std::ostringstream value;
    value << std::setw(2) << std::setfill('0') << month;
    return value.str();
}

std::string sourceName(bool prm) { return prm ? "PRM" : "PRD"; }

int telegramId(bool prm, int year, int month, int record)
{
    return (prm ? 100000000 : 200000000) +
        (year % 100) * 1000000 + month * 1000 + record;
}

int autoPadTelNo(int telegramId)
{
    return telegramId % 100000;
}

int syntheticLists(int telegramId)
{
    return static_cast<int>((telegramId * 48271LL + 17LL) % 9LL) + 1;
}

bool hasUnspacedEquals(const std::string& text)
{
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '=')
            continue;
        if (index == 0 || text[index - 1] != ' ' ||
            index + 1 >= text.size() || text[index + 1] != ' ')
        {
            return true;
        }
    }
    return false;
}

fs::path f12DatabaseRelativePath(int year)
{
    return fs::path(L"production") / L"F12" /
        (std::to_wstring(year) + L".db");
}

std::vector<Row> makeRows(
    bool prm,
    int year,
    int month,
    int count,
    const fs::path& logicalRoot,
    const fs::path& physicalRoot,
    const fs::path& relativeDirectory)
{
    std::vector<Row> rows;
    const std::string monthValue = monthText(month);
    const fs::path logicalDirectory = logicalRoot / relativeDirectory;
    const fs::path physicalDirectory = physicalRoot / relativeDirectory;
    fs::create_directories(physicalDirectory);

    for (int record = 1; record <= count; ++record) {
        Row row;
        row.id = telegramId(prm, year, month, record);
        row.prm = prm;
        row.month = month;
        row.record = record;
        std::ostringstream date;
        date << std::setw(2) << std::setfill('0') << ((record - 1) % 28 + 1)
             << '.' << monthValue << '.' << year;
        row.date = date.str();
        row.directTo = utf8(logicalDirectory) + "\\";
        row.fileName = std::to_string(row.id) +
            (prm ? "" : (record % 5 == 0 ? ".SHP" : ".ATL"));
        const std::string token = sourceName(prm) + "-" +
            std::to_string(year) + "-M" + monthValue + "-R" +
            std::to_string(record) + "-ID" + std::to_string(row.id);
        row.mainText =
            "SYNTHETIC-ARCHIVE-E2E " + token +
            " UNIQUEWORD_" + token +
            " CODE = " + std::to_string(row.id * 7LL + 31LL) +
            " DIGITAL = 3141592653 SEARCH-CONTROL\r\n";

        const int attachmentCount = record % 3;
        for (int attachment = 1; attachment <= attachmentCount; ++attachment) {
            Attachment item;
            item.name = sourceName(prm) + "_" + std::to_string(year) + "_" +
                monthValue + "_" + std::to_string(record) + "_APP" +
                std::to_string(attachment) + ".bin";
            item.size = static_cast<std::uint32_t>(
                192 + month * 17 + record * 13 + attachment * 31);
            item.security = "SYNTHETIC-GRIF";
            row.attachments.push_back(std::move(item));
        }

        writeText(physicalDirectory / encoding::utf8_to_wstring(row.fileName),
                  row.mainText);
        if (prm) {
            for (std::size_t index = 0; index < row.attachments.size(); ++index) {
                const auto& item = row.attachments[index];
                writeBytes(
                    physicalDirectory / encoding::utf8_to_wstring(item.name),
                    payload(item.size, static_cast<std::uint32_t>(row.id + index)));
            }
        } else if (!row.attachments.empty()) {
            std::vector<ZipEntry> entries;
            entries.push_back({
                "MESSAGE/aps/make_msg/tmpvl000.srg",
                std::vector<std::uint8_t>(row.mainText.begin(), row.mainText.end())});
            for (std::size_t index = 0; index < row.attachments.size(); ++index) {
                entries.push_back({
                    "MESSAGE/aps/make_msg/tmpvl00" +
                        std::to_string(index + 1) + ".srg",
                    payload(
                        row.attachments[index].size,
                        static_cast<std::uint32_t>(row.id + index))});
            }
            writeStoredZip(
                physicalDirectory / (std::to_wstring(row.id) + L".zip"),
                entries);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void bindText(sqlite3_stmt* statement, int index, const std::string& value)
{
    if (sqlite3_bind_text(
            statement, index, value.c_str(),
            static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
    {
        throw std::runtime_error("SQLite bind failed");
    }
}

void createDatabase(
    const fs::path& path,
    bool prm,
    const std::vector<Row>& rows)
{
    fs::create_directories(path.parent_path());
    Database database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    database.execute("PRAGMA journal_mode=DELETE");
    database.execute("PRAGMA synchronous=FULL");
    database.execute(createArchiveSql(prm ? prmColumns() : prdColumns()));
    database.execute("BEGIN IMMEDIATE");

    const std::string sql = prm
        ? "INSERT INTO ARCHIVE (\"Index\",DData,FFrom,TelNo,PodpNo,DataPodp,"
          "Familia,PrilName1,PrilName,KolPril,DirectTo,FileName,Copyes2,Edit,"
          "GdeSHT,SizeAll,Keys,Primechanie,Psekretno,Sekretno,Podrazd,"
          "Lists,Ekzempl) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        : "INSERT INTO ARCHIVE (\"Index\",DData,FFrom1,TelNo,PodpNo,DataPodp,"
          "Copyes,FFrom5,PrilName,KolPril,DirectTo,FileName,Blank,Edit,GdeSHT,"
          "SizeAll,SetevNo,FFrom2,FFrom3,AllPDTV1,Lists,Ekzempl) "
          "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &statement, nullptr) !=
        SQLITE_OK)
    {
        throw std::runtime_error("cannot prepare synthetic archive insert");
    }
    try {
        for (const auto& row : rows) {
            std::vector<std::string> names;
            std::vector<std::string> sizes;
            std::vector<std::string> securities;
            for (const auto& attachment : row.attachments) {
                names.push_back(attachment.name);
                sizes.push_back(std::to_string(attachment.size));
                securities.push_back(attachment.security);
            }
            if (sqlite3_bind_int(statement, 1, row.id) != SQLITE_OK)
                throw std::runtime_error("SQLite id bind failed");
            bindText(statement, 2, row.date);
            bindText(statement, 3, prm ? "SYNTHETIC-INCOMING" : "SYNTHETIC-OUTGOING");
            bindText(statement, 4, std::to_string(autoPadTelNo(row.id)));
            bindText(statement, 5, "SYN-" + std::to_string(row.id));
            bindText(statement, 6, row.date);
            bindText(statement, 7, prm ? "TEST-OPERATOR" : "TEST-COPIES");
            bindText(statement, 8, "UNIQUE-SUMMARY-" + std::to_string(row.id));
            bindText(statement, 9, prm ? joinTerminated(names, ';') : join(names, ';'));
            bindText(statement, 10, names.empty() ? "" : std::to_string(names.size()));
            bindText(statement, 11, row.directTo);
            bindText(statement, 12, row.fileName);
            bindText(statement, 13, "SYNTHETIC-BLANK-" + std::to_string(row.id));
            bindText(statement, 14, "0");
            bindText(statement, 15, "E2E-STAND");
            bindText(statement, 16, prm ? joinTerminated(sizes, ';') : join(sizes, ';'));
            bindText(statement, 17, prm ? "SYNTHETIC-KEY" : "TEST");
            bindText(statement, 18, prm ? "SYNTHETIC-ONLY" : "ATLAS-TEST");
            if (prm) {
                bindText(statement, 19, joinTerminated(securities, ';'));
                bindText(statement, 20, "SYNTHETIC-GRIF");
                bindText(statement, 21, "E2E-STAND");
            } else {
                bindText(statement, 19, "ISTOK-TEST");
                bindText(statement, 20, "");
            }
            const int listsIndex = prm ? 22 : 21;
            bindText(statement, listsIndex, std::to_string(syntheticLists(row.id)));
            bindText(statement, listsIndex + 1, "1");
            if (sqlite3_step(statement) != SQLITE_DONE)
                throw std::runtime_error("cannot insert synthetic archive row");
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
    } catch (...) {
        sqlite3_finalize(statement);
        database.execute("ROLLBACK");
        throw;
    }
    sqlite3_finalize(statement);
    database.execute("COMMIT");
    database.execute("PRAGMA wal_checkpoint(TRUNCATE)");
}

std::string issuanceTime(const Row& row)
{
    const int hour = 8 + (row.month + (row.prm ? 0 : 3)) % 10;
    const int minute = (row.record * 7 + row.month * 3) % 60;
    const int second = (row.record * 11 + row.month * 5) % 60;
    std::ostringstream value;
    value << std::setw(2) << std::setfill('0') << hour << ':'
          << std::setw(2) << minute << ':' << std::setw(2) << second;
    return value.str();
}

std::string issuanceRecipient(const Row& row)
{
    static const std::array<const wchar_t*, 4> recipients{
        L"Иванову", L"Петрову", L"Сидорову", L"В тестовый архив"};
    const std::size_t index = static_cast<std::size_t>(
        (row.month + row.record + (row.prm ? 0 : 1)) % recipients.size());
    return encoding::wstring_to_utf8(recipients[index]);
}

void createF12Database(
    const fs::path& path,
    int year,
    const std::vector<Row>& rows)
{
    fs::create_directories(path.parent_path());
    Database database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    database.execute("PRAGMA journal_mode=DELETE");
    database.execute("PRAGMA synchronous=FULL");
    database.execute(
        "CREATE TABLE app_schema_version ("
        "database_kind VARCHAR PRIMARY KEY, version INTEGER NOT NULL, "
        "migrated_at VARCHAR)");
    database.execute(
        "CREATE TABLE way ("
        "number INTEGER NOT NULL, ddate DATA NOT NULL, ttime DATA NOT NULL, "
        "kuda VARCHAR, type INTEGER NOT NULL, "
        "ind INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "str INTEGER, pos INTEGER, ekzempl INTEGER, "
        "page_owner VARCHAR, issued_to VARCHAR, source_tab_ind INTEGER, "
        "operator_name VARCHAR)");
    database.execute(
        "CREATE UNIQUE INDEX ux_way_source_tab_ind ON way(source_tab_ind)");

    sqlite3_stmt* version = nullptr;
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(
            database.get(),
            "INSERT INTO app_schema_version(database_kind,version,migrated_at) "
            "VALUES('way',?,?)", -1, &version, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            database.get(),
            "INSERT INTO way(number,ddate,ttime,kuda,type,str,pos,ekzempl,"
            "page_owner,issued_to,source_tab_ind,operator_name) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &insert, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(version);
        sqlite3_finalize(insert);
        throw std::runtime_error("cannot prepare synthetic F12 inserts");
    }

    database.execute("BEGIN IMMEDIATE");
    try {
        if (sqlite3_bind_int(version, 1, kF12WaySchemaVersion) != SQLITE_OK)
            throw std::runtime_error("cannot bind synthetic F12 schema version");
        bindText(version, 2, std::to_string(year) + "-01-01 00:00:00");
        if (sqlite3_step(version) != SQLITE_DONE)
            throw std::runtime_error("cannot insert synthetic F12 schema version");

        const std::string operatorName =
            encoding::wstring_to_utf8(L"ТЕСТОВЫЙ ОПЕРАТОР");
        for (const Row& row : rows) {
            const std::string recipient = issuanceRecipient(row);
            if (sqlite3_bind_int(insert, 1, row.id) != SQLITE_OK)
                throw std::runtime_error("cannot bind synthetic F12 number");
            bindText(insert, 2, row.date);
            bindText(insert, 3, issuanceTime(row));
            bindText(insert, 4, recipient);
            if (sqlite3_bind_int(insert, 5, row.prm ? 1 : 2) != SQLITE_OK ||
                sqlite3_bind_int(insert, 6, 600 + row.month) != SQLITE_OK ||
                sqlite3_bind_int(insert, 7, row.record) != SQLITE_OK ||
                sqlite3_bind_int(insert, 8, 1 + (row.record - 1) % 3) != SQLITE_OK)
            {
                throw std::runtime_error("cannot bind synthetic F12 issuance fields");
            }
            bindText(insert, 9, recipient);
            bindText(insert, 10, recipient);
            if (sqlite3_bind_int64(insert, 11, row.id) != SQLITE_OK)
                throw std::runtime_error("cannot bind synthetic F12 source row");
            bindText(insert, 12, operatorName);
            if (sqlite3_step(insert) != SQLITE_DONE)
                throw std::runtime_error("cannot insert synthetic F12 way row");
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        }
        database.execute("COMMIT");
    } catch (...) {
        database.execute("ROLLBACK");
        sqlite3_finalize(version);
        sqlite3_finalize(insert);
        throw;
    }
    sqlite3_finalize(version);
    sqlite3_finalize(insert);
}

void createSyntheticIndex(
    const fs::path& path,
    const fs::path& logicalRoot,
    const fs::path& physicalRoot,
    const std::vector<Row>& rows)
{
    fs::create_directories(path.parent_path());
    Database database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    database.execute("PRAGMA journal_mode=DELETE");
    database.execute("CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
    database.execute("CREATE TABLE words(word_id INTEGER PRIMARY KEY,word TEXT NOT NULL)");
    database.execute(
        "CREATE TABLE docs(doc_id INTEGER PRIMARY KEY,path TEXT NOT NULL,"
        "mtime_ticks INTEGER NOT NULL,size_int64 INTEGER NOT NULL,"
        "deleted INTEGER NOT NULL DEFAULT 0)");
    database.execute(
        "CREATE TABLE postings(word_id INTEGER NOT NULL,doc_id INTEGER NOT NULL,"
        "cnt INTEGER NOT NULL,PRIMARY KEY(word_id,doc_id)) WITHOUT ROWID");
    database.execute("CREATE UNIQUE INDEX idx_docs_path_unique ON docs(path)");
    database.execute("CREATE INDEX idx_docs_deleted ON docs(deleted)");
    database.execute("CREATE INDEX idx_postings_doc ON postings(doc_id)");
    database.execute("INSERT INTO meta(key,value) VALUES('schema_version','3')");
    database.execute(
        "INSERT INTO words(word_id,word) VALUES"
        "(0,'synthetic'),(1,'archive'),(2,'e2e'),(3,'search'),(4,'control')");

    sqlite3_stmt* insertDocument = nullptr;
    sqlite3_stmt* insertPosting = nullptr;
    if (sqlite3_prepare_v2(
            database.get(),
            "INSERT INTO docs(doc_id,path,mtime_ticks,size_int64,deleted) "
            "VALUES(?,?,0,?,0)",
            -1, &insertDocument, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            database.get(),
            "INSERT INTO postings(word_id,doc_id,cnt) VALUES(?,?,1)",
            -1, &insertPosting, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(insertDocument);
        sqlite3_finalize(insertPosting);
        throw std::runtime_error("cannot prepare synthetic index inserts");
    }

    database.execute("BEGIN IMMEDIATE");
    try {
        for (const auto& row : rows) {
            const fs::path logicalFile =
                fromUtf8(row.directTo) / encoding::utf8_to_wstring(row.fileName);
            const fs::path physicalFile = physicalPath(
                logicalFile, logicalRoot, physicalRoot);
            const std::string logicalUtf8 = utf8(logicalFile);
            sqlite3_bind_int(insertDocument, 1, row.id);
            sqlite3_bind_text(
                insertDocument, 2, logicalUtf8.c_str(),
                static_cast<int>(logicalUtf8.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(
                insertDocument, 3,
                static_cast<sqlite3_int64>(fs::file_size(physicalFile)));
            if (sqlite3_step(insertDocument) != SQLITE_DONE)
                throw std::runtime_error("cannot insert synthetic index document");
            sqlite3_reset(insertDocument);
            sqlite3_clear_bindings(insertDocument);

            for (int wordId = 0; wordId < 5; ++wordId) {
                sqlite3_bind_int(insertPosting, 1, wordId);
                sqlite3_bind_int(insertPosting, 2, row.id);
                if (sqlite3_step(insertPosting) != SQLITE_DONE)
                    throw std::runtime_error("cannot insert synthetic index posting");
                sqlite3_reset(insertPosting);
                sqlite3_clear_bindings(insertPosting);
            }
        }
        database.execute("COMMIT");
    } catch (...) {
        database.execute("ROLLBACK");
        sqlite3_finalize(insertDocument);
        sqlite3_finalize(insertPosting);
        throw;
    }
    sqlite3_finalize(insertDocument);
    sqlite3_finalize(insertPosting);
    database.execute("PRAGMA integrity_check");
}

json readJson(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open JSON: " + utf8(path));
    json value;
    input >> value;
    return value;
}

void writeJson(const fs::path& path, const json& value)
{
    writeText(path, value.dump(2) + "\n");
}

fs::path physicalPath(
    const fs::path& logicalPath,
    const fs::path& logicalRoot,
    const fs::path& physicalRoot)
{
    if (!isContained(logicalPath, logicalRoot))
        throw std::runtime_error("stand path escapes logical root: " + utf8(logicalPath));
    return physicalRoot / logicalPath.lexically_relative(logicalRoot);
}

void createSettings(
    const StandOptions& options,
    const fs::path& logicalRoot,
    const fs::path& physicalRoot,
    const ContentLayout& layout)
{
    json settings = readJson(options.settingsTemplate);
    json& config = settings["config"];
    const fs::path prm = logicalRoot / L"autopad" / L"PRM";
    const fs::path prd = logicalRoot / L"autopad" / L"PRD";
    const fs::path tlg = logicalRoot / L"content" / layout.tlgName;
    const fs::path production = logicalRoot / L"production";
    config["year"] = std::to_string(options.year);
    config["asio_port"] = 25000 + options.year % 100;
    config["server_mode"] = "active";
    config["document_catalog_storage"] = "sqlite";
    config["scan_on_startup"] = true;
    config["ind_time"] = 86400;
    config["index_roots"] = json::array();
    for (const auto& monthName : layout.prmMonthNames) {
        config["index_roots"].push_back(
            utf8(logicalRoot / L"content" / monthName));
    }
    config["index_roots"].push_back(utf8(tlg));
    config["excluded_subtrees"] = json::array({utf8(tlg / L"OUT")});
    config["prm_base_dir"] = utf8(prm);
    config["prd_base_dir"] = utf8(prd);
    config["prm_monthly_bases_dir"] = utf8(prm / L"METH_BASES");
    config["prd_monthly_bases_dir"] = utf8(prd / L"METH_BASES");
    config["tlg_send_root"] = utf8(tlg);
    config["razn_output_dir"] = utf8(production / L"RAZN");
    config["opis_base_dir"] = utf8(production / L"OPIS");
    config["f12_base_dir"] = utf8(production / L"F12");
    for (const wchar_t* directory : {L"RAZN", L"OPIS", L"F12"})
        fs::create_directories(physicalRoot / L"production" / directory);
    const fs::path data = physicalRoot / L"server" / L"data";
    writeJson(data / L"Settings.json", settings);
    writeText(data / L"ignore.txt", "# Synthetic E2E stand\r\n");
    writeText(data / L"prefix_map.json", "{}\r\n");
}

void freezeServiceArchiveSettings(
    const fs::path& physicalRoot,
    const fs::path& logicalRoot,
    const fs::path& restoreRoot,
    int port)
{
    const fs::path data = physicalRoot / L"server" / L"data";
    const fs::path settingsPath = data / L"Settings.json";
    json settings = readJson(settingsPath);
    json& config = settings["config"];
    config["server_mode"] = "archive";
    config["asio_port"] = port;
    config["document_catalog_storage"] = "sqlite";
    config["scan_on_startup"] = false;
    config["prm_base_dir"] = "";
    config["prd_base_dir"] = "";
    config["prm_monthly_bases_dir"] =
        utf8(logicalRoot / L"autopad" / L"PRM" / L"METH_BASES");
    config["prd_monthly_bases_dir"] =
        utf8(logicalRoot / L"autopad" / L"PRD" / L"METH_BASES");
    // These four paths are not rewritten by the released legacy restore tool.
    // Store their future active values up front so the same archive remains
    // restorable without replacing SearchEngineArchive.exe on the target VM.
    config["tlg_send_root"] = utf8(restoreRoot / L"content" / L"TLG");
    config["razn_output_dir"] = utf8(restoreRoot / L"production" / L"RAZN");
    config["opis_base_dir"] = utf8(restoreRoot / L"production" / L"OPIS");
    config["f12_base_dir"] = utf8(restoreRoot / L"production" / L"F12");
    writeJson(settingsPath, settings);
}

std::wstring safeServiceName(const std::wstring& value)
{
    if (value.empty() || value.size() > 128 || !std::iswalnum(value.front()))
        throw std::invalid_argument("service name is invalid");
    for (wchar_t ch : value) {
        if (!std::iswalnum(ch) && ch != L'-' && ch != L'_')
            throw std::invalid_argument("service name is invalid");
    }
    return value;
}

std::wstring quoteCommandArgument(const fs::path& value)
{
    std::wstring text = value.wstring();
    if (text.find(L'\"') != std::wstring::npos)
        throw std::invalid_argument("service path contains a quote");
    return L"\"" + text + L"\"";
}

std::wstring quoteCommandArgument(const std::wstring& value)
{
    if (value.find(L'\"') != std::wstring::npos)
        throw std::invalid_argument("service name contains a quote");
    return L"\"" + value + L"\"";
}

std::wstring serviceImagePath(
    const fs::path& executable,
    const std::wstring& serviceName,
    const fs::path& dataDirectory)
{
    return quoteCommandArgument(executable) +
        L" --service --service-name " + quoteCommandArgument(serviceName) +
        L" --data-dir " + quoteCommandArgument(dataDirectory);
}

void appendMappedFiles(
    json& files,
    const fs::path& physicalTargetRoot,
    const fs::path& logicalTargetRoot,
    const fs::path& logicalSourceRoot)
{
    for (fs::recursive_directory_iterator it(physicalTargetRoot), end;
         it != end; ++it)
    {
        if (!it->is_regular_file())
            continue;
        const fs::path relative = it->path().lexically_relative(physicalTargetRoot);
        const FileHashResult hash = sha256File(it->path());
        if (!hash.ok)
            throw std::runtime_error(hash.message);
        files.push_back({
            {"source", utf8(logicalSourceRoot / relative)},
            {"target", utf8(logicalTargetRoot / relative)},
            {"source_size", hash.size},
            {"source_sha256", hash.sha256},
            {"target_size", hash.size},
            {"target_sha256", hash.sha256},
            {"transformed", false}});
    }
}

void writeActivationScript(
    const fs::path& physicalRoot,
    const std::wstring& serviceName)
{
    const std::string service = encoding::wstring_to_utf8(serviceName);
    const std::string escapedImage =
        "\\\"%STAND_ROOT%\\server\\program\\SearchEngine.exe\\\" "
        "--service --service-name \\\"" + service +
        "\\\" --data-dir \\\"%STAND_ROOT%\\server\\data\\\"";
    std::ostringstream script;
    script
        << "@echo off\r\n"
        << "setlocal EnableExtensions DisableDelayedExpansion\r\n"
        << "for %%I in (\"%~dp0.\") do set \"STAND_ROOT=%%~fI\"\r\n"
        << "call \"%STAND_ROOT%\\Prepare-Archived-Stand.bat\"\r\n"
        << "if errorlevel 1 (\r\n"
        << "  echo ERROR: portable stand preparation failed.\r\n"
        << "  pause\r\n  exit /b 1\r\n)\r\n"
        << "fsutil.exe dirty query %SystemDrive% >nul 2>&1\r\n"
        << "if errorlevel 1 (\r\n"
        << "  echo ERROR: run this file as Administrator.\r\n"
        << "  pause\r\n  exit /b 1\r\n)\r\n"
        << "sc.exe query \"" << service << "\" >nul 2>&1\r\n"
        << "if not errorlevel 1 (\r\n"
        << "  echo ERROR: test service already exists: " << service << "\r\n"
        << "  pause\r\n  exit /b 1\r\n)\r\n"
        << "sc.exe create \"" << service << "\" binPath= \"" << escapedImage
        << "\" start= demand DisplayName= \"SearchEngine archive stand v3\"\r\n"
        << "if errorlevel 1 goto :failed\r\n"
        << "sc.exe start \"" << service << "\" >nul 2>&1\r\n"
        << "set /a WAIT_SECONDS=0\r\n"
        << ":wait_running\r\n"
        << "sc.exe query \"" << service
        << "\" | findstr.exe /R /C:\"[ ]4[ ]*RUNNING\" >nul\r\n"
        << "if not errorlevel 1 goto :success\r\n"
        << "set /a WAIT_SECONDS+=1\r\n"
        << "if %WAIT_SECONDS% GEQ 120 goto :failed\r\n"
        << "ping.exe 127.0.0.1 -n 2 >nul\r\n"
        << "goto :wait_running\r\n"
        << ":success\r\n"
        << "echo Test archive service is RUNNING: " << service << "\r\n"
        << "echo Now run Archive-SearchEngineService.bat and select 2 then 2.\r\n"
        << "pause\r\nexit /b 0\r\n"
        << ":failed\r\n"
        << "echo ERROR: test archive service did not reach RUNNING.\r\n"
        << "sc.exe query \"" << service << "\"\r\n"
        << "pause\r\nexit /b 1\r\n";
    writeText(physicalRoot / L"Activate-Archived-Stand.bat", script.str());
}

void writePreparationScript(const fs::path& physicalRoot)
{
    std::ostringstream script;
    script
        << "@echo off\r\n"
        << "setlocal EnableExtensions DisableDelayedExpansion\r\n"
        << "for %%I in (\"%~dp0.\") do set \"STAND_ROOT=%%~fI\"\r\n"
        << "set \"PREPARER=%STAND_ROOT%\\tools\\SearchEngineArchiveE2EStand.exe\"\r\n"
        << "if not exist \"%PREPARER%\" (\r\n"
        << "  echo ERROR: portable stand preparer is missing: %PREPARER%\r\n"
        << "  exit /b 1\r\n)\r\n"
        << "\"%PREPARER%\" prepare-service-archive --root \"%STAND_ROOT%\"\r\n"
        << "if errorlevel 1 exit /b 1\r\n"
        << "exit /b 0\r\n";
    writeText(physicalRoot / L"Prepare-Archived-Stand.bat", script.str());
}

void writeWorkstationDeploymentScript(
    const fs::path& physicalRoot,
    const std::wstring& serviceName,
    int port,
    int year)
{
    const std::string service = encoding::wstring_to_utf8(serviceName);
    const std::string escapedImage =
        "\\\"%INSTALL_ROOT%\\bin\\SearchEngine.exe\\\" "
        "--service --service-name \\\"" + service +
        "\\\" --data-dir \\\"%DATA_ROOT%\\\"";
    std::ostringstream script;
    script
        << "@echo off\r\n"
        << "setlocal EnableExtensions DisableDelayedExpansion\r\n"
        << "for %%I in (\"%~dp0.\") do set \"STAND_ROOT=%%~fI\"\r\n"
        << "set \"PREPARER=%STAND_ROOT%\\tools\\SearchEngineArchiveE2EStand.exe\"\r\n"
        << "set \"REDIST=%STAND_ROOT%\\installer\\prerequisites\\vc_redist.x86.exe\"\r\n"
        << "if not exist \"%PREPARER%\" goto :package_missing\r\n"
        << "if not exist \"%REDIST%\" goto :package_missing\r\n"
        << "if not exist \"%STAND_ROOT%\\installer\\tools\\SearchEngineConfig.exe\" goto :package_missing\r\n"
        << "if not exist \"%STAND_ROOT%\\installer\\Verify-Package.bat\" goto :package_missing\r\n"
        << "call \"%STAND_ROOT%\\installer\\Verify-Package.bat\" /quiet\r\n"
        << "if errorlevel 1 goto :package_damaged\r\n"
        << "fsutil.exe dirty query %SystemDrive% >nul 2>&1\r\n"
        << "if errorlevel 1 goto :not_admin\r\n"
        << "sc.exe query \"" << service << "\" >nul 2>&1\r\n"
        << "if not errorlevel 1 goto :service_exists\r\n"
        << "set \"DATA_DRIVE=\"\r\n"
        << "set /P \"DATA_DRIVE=Data volume letter (example D:): \"\r\n"
        << "if \"%DATA_DRIVE:~1,1%\"==\"\" set \"DATA_DRIVE=%DATA_DRIVE%:\"\r\n"
        << "echo(%DATA_DRIVE%| findstr.exe /R /X \"[A-Za-z]:\" >nul\r\n"
        << "if errorlevel 1 goto :bad_drive\r\n"
        << "if not exist \"%DATA_DRIVE%\\\" goto :bad_drive\r\n"
        << "set \"PROGRAM_ROOT=%ProgramFiles%\"\r\n"
        << "if not \"%ProgramFiles(x86)%\"==\"\" set \"PROGRAM_ROOT=%ProgramFiles(x86)%\"\r\n"
        << "set \"INSTALL_ROOT=%PROGRAM_ROOT%\\" << service << "\"\r\n"
        << "set \"DATA_ROOT=%ProgramData%\\" << service << "\"\r\n"
        << "echo Installing or updating Microsoft Visual C++ Runtime...\r\n"
        << "start \"\" /wait \"%REDIST%\" /install /quiet /norestart\r\n"
        << "set \"REDIST_EXIT=%ERRORLEVEL%\"\r\n"
        << "if \"%REDIST_EXIT%\"==\"0\" goto :redist_ok\r\n"
        << "if \"%REDIST_EXIT%\"==\"1638\" goto :redist_ok\r\n"
        << "if \"%REDIST_EXIT%\"==\"3010\" goto :redist_ok\r\n"
        << "echo ERROR: Visual C++ Runtime setup failed: %REDIST_EXIT%\r\n"
        << "goto :failed\r\n"
        << ":redist_ok\r\n"
        << "\"%PREPARER%\" deploy-workstation-stand --root \"%STAND_ROOT%\" --data-volume-root \"%DATA_DRIVE%\\.\" --program-files-root \"%PROGRAM_ROOT%\" --program-data-root \"%ProgramData%\"\r\n"
        << "if errorlevel 1 goto :failed\r\n"
        << "> \"%DATA_ROOT%\\client-endpoint.txt\" echo server_id=default\r\n"
        << ">> \"%DATA_ROOT%\\client-endpoint.txt\" echo display_name=Search Engine ASIO Server ^(archive stand^)\r\n"
        << ">> \"%DATA_ROOT%\\client-endpoint.txt\" echo host=%COMPUTERNAME%\r\n"
        << ">> \"%DATA_ROOT%\\client-endpoint.txt\" echo god=" << year << "\r\n"
        << ">> \"%DATA_ROOT%\\client-endpoint.txt\" echo port=" << port << "\r\n"
        << ">> \"%DATA_ROOT%\\client-endpoint.txt\" echo service_name=" << service << "\r\n"
        << "sc.exe create \"" << service << "\" binPath= \"" << escapedImage
        << "\" start= delayed-auto DisplayName= \"SearchEngine workstation stand\"\r\n"
        << "if errorlevel 1 goto :failed\r\n"
        << "netsh.exe advfirewall firewall delete rule name=\"" << service << " TCP\" >nul 2>&1\r\n"
        << "netsh.exe advfirewall firewall add rule name=\"" << service << " TCP\" dir=in action=allow protocol=TCP localport=" << port << " program=\"%INSTALL_ROOT%\\bin\\SearchEngine.exe\" enable=yes >nul\r\n"
        << "if errorlevel 1 goto :failed\r\n"
        << "sc.exe start \"" << service << "\" >nul 2>&1\r\n"
        << "set /a WAIT_SECONDS=0\r\n"
        << ":wait_running\r\n"
        << "sc.exe query \"" << service << "\" | findstr.exe /R /C:\"[ ]4[ ]*RUNNING\" >nul\r\n"
        << "if not errorlevel 1 goto :health\r\n"
        << "set /a WAIT_SECONDS+=1\r\n"
        << "if %WAIT_SECONDS% GEQ 120 goto :failed\r\n"
        << "ping.exe 127.0.0.1 -n 2 >nul\r\n"
        << "goto :wait_running\r\n"
        << ":health\r\n"
        << "\"%INSTALL_ROOT%\\tools\\SearchEngineConfig.exe\" health --port " << port << " --timeout-ms 10000 >nul 2>&1\r\n"
        << "if errorlevel 1 goto :failed\r\n"
        << "echo Workstation-like archive stand is RUNNING.\r\n"
        << "echo Program: %INSTALL_ROOT%\\bin\r\n"
        << "echo Data:    %DATA_ROOT%\r\n"
        << "echo Volume:  %DATA_DRIVE%\\\r\n"
        << "pause\r\nexit /b 0\r\n"
        << ":package_missing\r\n"
        << "echo ERROR: workstation deployment files are missing.\r\n"
        << "goto :failed\r\n"
        << ":package_damaged\r\n"
        << "echo ERROR: bundled portable installer failed checksum verification.\r\n"
        << "goto :failed\r\n"
        << ":not_admin\r\n"
        << "echo ERROR: run this file as Administrator.\r\n"
        << "goto :failed\r\n"
        << ":service_exists\r\n"
        << "echo ERROR: test service already exists: " << service << "\r\n"
        << "goto :failed\r\n"
        << ":bad_drive\r\n"
        << "echo ERROR: enter an existing drive letter such as D:.\r\n"
        << ":failed\r\n"
        << "echo Deployment stopped. Existing files were preserved for inspection.\r\n"
        << "pause\r\nexit /b 1\r\n";
    writeText(
        physicalRoot / L"Deploy-Workstation-Stand.bat",
        script.str());
}

void writeWorkstationReadme(
    const fs::path& physicalRoot,
    const std::wstring& serviceName)
{
    std::ostringstream text;
    text << "\xEF\xBB\xBF"
         << "Дополнительный режим: чистая тестовая VM\r\n"
         << "========================================\r\n\r\n"
         << "Существующий Activate-Archived-Stand.bat не изменён.\r\n"
         << "Для раскладки как на рабочей машине запустите от администратора\r\n"
         << "Deploy-Workstation-Stand.bat и укажите букву отдельного тома.\r\n\r\n"
         << "Служба: " << encoding::wstring_to_utf8(serviceName) << "\r\n"
         << "Программа: %ProgramFiles(x86)%\\<служба>\\bin и tools\r\n"
         << "Настройки: %ProgramData%\\<служба>\r\n"
         << "На выбранном томе: месяцы, TLG, BASES, BASES_PRD, "
            "OPIS_ADMIN и F12.\r\n\r\n"
         << "Режим предназначен для чистой одноразовой VM. Он откажется\r\n"
         << "работать, если служба или любой целевой каталог уже существует.\r\n";
    writeText(
        physicalRoot / L"README-WORKSTATION-RU.txt",
        text.str());
}

void writeServiceArchiveReadme(
    const fs::path& physicalRoot,
    const fs::path& logicalRoot,
    const fs::path& restoreRoot,
    const std::wstring& serviceName,
    int port)
{
    std::ostringstream text;
    text << "\xEF\xBB\xBF"
         << "SearchEngine: синтетический архивный стенд v3\r\n"
         << "===============================================\r\n\r\n"
         << "Служба: " << encoding::wstring_to_utf8(serviceName) << "\r\n"
         << "Порт: " << port << "\r\n"
         << "Исходный шаблон пути: " << utf8(logicalRoot) << "\r\n"
         << "Исходный шаблон возврата: " << utf8(restoreRoot) << "\r\n\r\n"
         << "1. Распакуйте каталог в любую папку/на любой диск, не меняя имя "
            "SearchEngineService-StandV3-2026.\r\n"
         << "2. От имени администратора запустите Activate-Archived-Stand.bat. "
            "Он сам перепривяжет все пути.\r\n"
         << "3. Запустите D:\\ss-install\\Archive-SearchEngineService.bat.\r\n"
         << "4. Выберите: 2 (операции со службой), затем 2 (вернуть).\r\n"
         << "5. Выберите ИМЕННО фактический каталог архивированной службы.\r\n"
         << "6. Выберите способ разморозки:\r\n"
         << "   1 — вернуть в записанные исходные места 1:1. Используйте "
            "для архива, созданного после Deploy-Workstation-Stand.bat;\r\n"
         << "   2 — восстановить всё под выбранный каталог. Затем можно "
            "ввести D:, D:\\ или полный путь D:\\StandV3.\r\n"
         << "7. Подтвердите восстановление: y.\r\n"
         << "8. При первом проходе на вопрос об удалении архива ответьте N.\r\n\r\n"
         << "Режим 1 возвращает программу в Program Files, настройки и индекс "
            "в ProgramData, а TLG и базы — в записанные рабочие каталоги. "
            "F12 и OPIS_ADMIN не затрагиваются.\r\n"
         << "Режим 2 сохраняет переносимую раскладку program/data/content/"
            "autopad под одним выбранным корнем.\r\n\r\n"
         << "archive-operation.json — рабочий манифест штатной утилиты.\r\n"
         << "stand-manifest.json — дополнительный паспорт синтетических данных.\r\n"
         << "Все данные искусственные; производственные файлы не использованы.\r\n";
    writeText(physicalRoot / L"README-V3-RU.txt", text.str());
}

void writeServiceArchiveManifest(
    const ServiceArchiveStandOptions& options,
    const fs::path& physicalRoot,
    const fs::path& logicalRoot,
    const fs::path& restoreRoot,
    const json& standManifest)
{
    const std::wstring serviceName = safeServiceName(options.serviceName);
    const fs::path archivedProgram = logicalRoot / L"server" / L"program";
    const fs::path archivedData = logicalRoot / L"server" / L"data";
    const fs::path originalProgram = restoreRoot / L"server" / L"program";
    const fs::path originalData = restoreRoot / L"server" / L"data";
    json manifest{
        {"format_version", 1},
        {"operation", "service-archive"},
        {"phase", "archive-running-source-cleaned"},
        {"year", options.stand.year},
        {"service_name", encoding::wstring_to_utf8(serviceName)},
        {"original_image_path", encoding::wstring_to_utf8(serviceImagePath(
            originalProgram / L"SearchEngine.exe", serviceName, originalData))},
        {"archived_image_path", encoding::wstring_to_utf8(serviceImagePath(
            archivedProgram / L"SearchEngine.exe", serviceName, archivedData))},
        {"original_executable", utf8(originalProgram / L"SearchEngine.exe")},
        {"archived_executable", utf8(archivedProgram / L"SearchEngine.exe")},
        {"original_data_directory", utf8(originalData)},
        {"archived_data_directory", utf8(archivedData)},
        {"original_prm_monthly_directory", utf8(
            restoreRoot / L"autopad" / L"PRM" / L"METH_BASES")},
        {"original_prd_monthly_directory", utf8(
            restoreRoot / L"autopad" / L"PRD" / L"METH_BASES")},
        {"warnings", json::array({
            "synthetic disposable E2E stand; no production data"})},
        {"mappings", json::array()},
        {"files", json::array()},
        {"monthly_databases", json::array()}};

    const std::array<std::pair<fs::path, fs::path>, 4> mappings{{
        {originalProgram, archivedProgram},
        {originalData, archivedData},
        {restoreRoot / L"content", logicalRoot / L"content"},
        {restoreRoot / L"production", logicalRoot / L"production"}}};
    for (const auto& mapping : mappings) {
        manifest["mappings"].push_back({
            {"source", utf8(mapping.first)},
            {"target", utf8(mapping.second)}});
        appendMappedFiles(
            manifest["files"],
            physicalPath(mapping.second, logicalRoot, physicalRoot),
            mapping.second,
            mapping.first);
    }

    for (const auto& item : standManifest.at("databases")) {
        const fs::path relative = fromUtf8(
            item.at("relative_path").get<std::string>());
        const fs::path physical = physicalRoot / relative;
        const fs::path target = logicalRoot / relative;
        const fs::path source = restoreRoot / relative;
        const FileHashResult hash = sha256File(physical);
        if (!hash.ok)
            throw std::runtime_error(hash.message);
        manifest["monthly_databases"].push_back({
            {"kind", item.at("source")},
            {"month", item.at("month")},
            {"source", utf8(source)},
            {"target", utf8(target)},
            {"source_fingerprint", "synthetic-source-cleaned"},
            {"source_fingerprint_cacheable", false},
            {"source_journal_mode", "delete"},
            {"source_size", hash.size},
            {"source_sha256", hash.sha256},
            {"target_size", hash.size},
            {"target_sha256", hash.sha256}});
    }
    writeJson(physicalRoot / kArchiveManifestName, manifest);
}

std::string sqliteText(sqlite3_stmt* statement, int column)
{
    const auto* text = sqlite3_column_text(statement, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

void verifyColumns(sqlite3* database, bool prm)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(ARCHIVE)", -1,
                          &statement, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("cannot read synthetic archive schema");
    }
    std::vector<std::string> actual;
    while (sqlite3_step(statement) == SQLITE_ROW)
        actual.push_back(sqliteText(statement, 1));
    sqlite3_finalize(statement);
    const auto& expected = prm ? prmColumns() : prdColumns();
    if (actual.size() != expected.size())
        throw std::runtime_error("synthetic archive column count mismatch");
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != expected[index].first)
            throw std::runtime_error("synthetic archive column order mismatch");
    }
}

void verifyIntegrity(sqlite3* database)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1,
                          &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW || sqliteText(statement, 0) != "ok")
    {
        sqlite3_finalize(statement);
        throw std::runtime_error("synthetic SQLite integrity check failed");
    }
    sqlite3_finalize(statement);
}

struct WayExpectation {
    int number{};
    int type{};
};

using WayExpectations = std::map<sqlite3_int64, WayExpectation>;

void verifyF12Database(
    const fs::path& path,
    int year,
    const WayExpectations& expected)
{
    Database database(path, SQLITE_OPEN_READONLY);
    database.execute("PRAGMA query_only=ON");
    verifyIntegrity(database.get());

    sqlite3_stmt* columns = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), "PRAGMA table_info(way)", -1,
            &columns, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("cannot read synthetic F12 way schema");
    }
    const std::array<const char*, 13> expectedColumns{
        "number", "ddate", "ttime", "kuda", "type", "ind", "str",
        "pos", "ekzempl", "page_owner", "issued_to", "source_tab_ind",
        "operator_name"};
    std::size_t columnIndex = 0;
    while (sqlite3_step(columns) == SQLITE_ROW) {
        if (columnIndex >= expectedColumns.size() ||
            sqliteText(columns, 1) != expectedColumns[columnIndex])
        {
            sqlite3_finalize(columns);
            throw std::runtime_error("synthetic F12 way column mismatch");
        }
        ++columnIndex;
    }
    sqlite3_finalize(columns);
    if (columnIndex != expectedColumns.size())
        throw std::runtime_error("synthetic F12 way column count mismatch");

    sqlite3_stmt* version = nullptr;
    if (sqlite3_prepare_v2(
            database.get(),
            "SELECT version FROM app_schema_version "
            "WHERE database_kind='way'",
            -1, &version, nullptr) != SQLITE_OK ||
        sqlite3_step(version) != SQLITE_ROW ||
        sqlite3_column_int(version, 0) != kF12WaySchemaVersion ||
        sqlite3_step(version) != SQLITE_DONE)
    {
        sqlite3_finalize(version);
        throw std::runtime_error("synthetic F12 way schema version mismatch");
    }
    sqlite3_finalize(version);

    sqlite3_stmt* indexes = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), "PRAGMA index_list(way)", -1,
            &indexes, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("cannot read synthetic F12 way indexes");
    }
    bool uniqueSourceIndex = false;
    while (sqlite3_step(indexes) == SQLITE_ROW) {
        if (sqliteText(indexes, 1) == "ux_way_source_tab_ind" &&
            sqlite3_column_int(indexes, 2) == 1)
        {
            uniqueSourceIndex = true;
        }
    }
    sqlite3_finalize(indexes);
    if (!uniqueSourceIndex)
        throw std::runtime_error("synthetic F12 source index is missing");

    sqlite3_stmt* rows = nullptr;
    const char sql[] =
        "SELECT number,type,source_tab_ind,ddate,ttime,kuda,page_owner,"
        "issued_to,operator_name FROM way ORDER BY source_tab_ind";
    if (sqlite3_prepare_v2(
            database.get(), sql, -1, &rows, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error("cannot read synthetic F12 way rows");
    }
    std::set<sqlite3_int64> actualSources;
    const std::string yearText = std::to_string(year);
    while (sqlite3_step(rows) == SQLITE_ROW) {
        const int number = sqlite3_column_int(rows, 0);
        const int type = sqlite3_column_int(rows, 1);
        const sqlite3_int64 source = sqlite3_column_int64(rows, 2);
        const auto expectedRow = expected.find(source);
        if (expectedRow == expected.end() ||
            expectedRow->second.number != number ||
            expectedRow->second.type != type ||
            !actualSources.insert(source).second ||
            sqliteText(rows, 3).find(yearText) == std::string::npos ||
            sqliteText(rows, 4).empty() || sqliteText(rows, 5).empty() ||
            sqliteText(rows, 5) != sqliteText(rows, 6) ||
            sqliteText(rows, 5) != sqliteText(rows, 7) ||
            sqliteText(rows, 8).empty())
        {
            sqlite3_finalize(rows);
            throw std::runtime_error(
                "synthetic F12 row is not linked to its telegram");
        }
    }
    sqlite3_finalize(rows);
    if (actualSources.size() != expected.size())
        throw std::runtime_error("synthetic F12 way row count mismatch");
}

StandSummary verifyInternal(
    const fs::path& physicalRoot,
    const fs::path& logicalRoot,
    const json& manifest)
{
    StandSummary summary;
    summary.root = logicalRoot;
    summary.year = manifest.at("year").get<int>();
    const int recordsPerMonth = manifest.at("records_per_month").get<int>();
    const json& contentLayout = manifest.at("content_layout");
    const json& prmMonthNames = contentLayout.at("prm_month_directories");
    if (!prmMonthNames.is_array() || prmMonthNames.size() != 12)
        throw std::runtime_error("synthetic content layout has no 12 PRM months");
    const fs::path tlgName = fromUtf8(
        contentLayout.at("prd_tlg_directory").get<std::string>());
    std::set<int> uniqueIds;
    WayExpectations expectedWayRows;

    for (const auto& item : manifest.at("databases")) {
        const bool prm = item.at("source").get<std::string>() == "PRM";
        const int month = item.at("month").get<int>();
        const fs::path expectedLogicalDirectory = logicalRoot / L"content" /
            (prm
                ? fromUtf8(prmMonthNames.at(month - 1).get<std::string>())
                : tlgName);
        const fs::path databasePath =
            physicalRoot / fromUtf8(item.at("relative_path").get<std::string>());
        Database database(databasePath, SQLITE_OPEN_READONLY);
        database.execute("PRAGMA query_only=ON");
        verifyIntegrity(database.get());
        verifyColumns(database.get(), prm);
        sqlite3_stmt* statement = nullptr;
        const char sql[] =
            "SELECT \"Index\",TelNo,KolPril,PrilName,SizeAll,DirectTo,FileName,"
            "Lists,Ekzempl,Psekretno,Sekretno,Podrazd "
            "FROM ARCHIVE ORDER BY \"Index\"";
        if (sqlite3_prepare_v2(database.get(), sql, -1, &statement, nullptr) !=
            SQLITE_OK)
        {
            throw std::runtime_error("cannot read synthetic archive rows");
        }
        int rowCount = 0;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            ++rowCount;
            ++summary.telegramRowCount;
            const int id = sqlite3_column_int(statement, 0);
            const int telNo = sqlite3_column_int(statement, 1);
            const std::string declaredText = sqliteText(statement, 2);
            const int declaredCount = declaredText.empty()
                ? 0 : std::stoi(declaredText);
            const auto names = prm
                ? splitTerminatedF12List(sqliteText(statement, 3))
                : split(sqliteText(statement, 3), ';');
            const auto sizes = prm
                ? splitTerminatedF12List(sqliteText(statement, 4))
                : split(sqliteText(statement, 4), ';');
            const auto securities = prm
                ? splitTerminatedF12List(sqliteText(statement, 9))
                : std::vector<std::string>{};
            const int lists = sqlite3_column_int(statement, 7);
            const int ekzempl = sqlite3_column_int(statement, 8);
            if (telNo != autoPadTelNo(id) ||
                declaredCount != static_cast<int>(names.size()) ||
                names.size() != sizes.size() ||
                (prm && (names.size() != securities.size() ||
                    sqliteText(statement, 10).empty() ||
                    sqliteText(statement, 11).empty())) ||
                lists != syntheticLists(id) || ekzempl != 1)
            {
                sqlite3_finalize(statement);
                throw std::runtime_error(
                    "synthetic archive row contract mismatch");
            }
            const fs::path logicalDirectory = fromUtf8(sqliteText(statement, 5));
            const fs::path fileName = fromUtf8(sqliteText(statement, 6));
            if (!fileName.has_filename() || fileName.has_parent_path() ||
                !isContained(logicalDirectory, logicalRoot) ||
                !isContained(logicalDirectory, expectedLogicalDirectory) ||
                !isContained(expectedLogicalDirectory, logicalDirectory))
            {
                sqlite3_finalize(statement);
                throw std::runtime_error("unsafe synthetic DirectTo/FileName");
            }
            const fs::path directory = physicalPath(
                logicalDirectory, logicalRoot, physicalRoot);
            const fs::path mainFile = directory / fileName;
            if (!fs::is_regular_file(mainFile)) {
                sqlite3_finalize(statement);
                throw std::runtime_error("synthetic telegram file is missing");
            }
            std::ifstream mainInput(mainFile, std::ios::binary);
            const std::string mainText{
                std::istreambuf_iterator<char>(mainInput),
                std::istreambuf_iterator<char>()};
            if (mainText.find("SYNTHETIC-ARCHIVE-E2E") == std::string::npos ||
                mainText.find(std::to_string(id)) == std::string::npos ||
                mainText.find(" CODE = ") == std::string::npos ||
                mainText.find(" DIGITAL = 3141592653 ") == std::string::npos ||
                hasUnspacedEquals(mainText))
            {
                sqlite3_finalize(statement);
                throw std::runtime_error(
                    "synthetic telegram text contract mismatch");
            }

            const bool firstReference = uniqueIds.insert(id).second;
            if (firstReference) {
                ++summary.uniqueTelegramCount;
                summary.attachmentCount += declaredCount;
            }
            if (!expectedWayRows.emplace(
                    static_cast<sqlite3_int64>(id),
                    WayExpectation{id, prm ? 1 : 2}).second)
            {
                sqlite3_finalize(statement);
                throw std::runtime_error(
                    "duplicate synthetic source row for F12");
            }
            if (prm) {
                for (std::size_t index = 0; index < names.size(); ++index) {
                    const fs::path attachment =
                        directory / encoding::utf8_to_wstring(names[index]);
                    if (!fs::is_regular_file(attachment) ||
                        fs::file_size(attachment) !=
                            static_cast<std::uintmax_t>(std::stoull(sizes[index])))
                    {
                        sqlite3_finalize(statement);
                        throw std::runtime_error("PRM attachment size mismatch");
                    }
                }
            } else if (!names.empty()) {
                const auto entries = readStoredZipEntries(
                    directory / (std::to_wstring(id) + L".zip"));
                if (entries.size() != names.size() + 1 ||
                    entries.front().first != "MESSAGE/aps/make_msg/tmpvl000.srg")
                {
                    sqlite3_finalize(statement);
                    throw std::runtime_error("PRD ZIP payload count mismatch");
                }
                for (std::size_t index = 0; index < names.size(); ++index) {
                    const std::string expectedName =
                        "MESSAGE/aps/make_msg/tmpvl00" +
                        std::to_string(index + 1) + ".srg";
                    if (entries[index + 1].first != expectedName ||
                        entries[index + 1].second != std::stoul(sizes[index]))
                    {
                        sqlite3_finalize(statement);
                        throw std::runtime_error("PRD ZIP attachment size mismatch");
                    }
                }
            }
        }
        sqlite3_finalize(statement);
        if (rowCount != recordsPerMonth)
            throw std::runtime_error("synthetic database row count mismatch");
        ++summary.databaseCount;
    }

    if (summary.databaseCount != 24 ||
        summary.uniqueTelegramCount != recordsPerMonth * 24)
    {
        throw std::runtime_error("synthetic stand inventory mismatch");
    }
    if (manifest.contains("f12")) {
        const json& f12 = manifest.at("f12");
        const fs::path relative = fromUtf8(
            f12.at("relative_path").get<std::string>());
        if (relative != f12DatabaseRelativePath(summary.year) ||
            f12.at("schema_version").get<int>() != kF12WaySchemaVersion ||
            f12.at("rows").get<int>() != recordsPerMonth * 24 ||
            f12.at("prm_type").get<int>() != 1 ||
            f12.at("prd_type").get<int>() != 2)
        {
            throw std::runtime_error("synthetic F12 manifest mismatch");
        }
        verifyF12Database(
            physicalRoot / relative, summary.year, expectedWayRows);
        summary.f12WayRowCount = static_cast<int>(expectedWayRows.size());
    }
    const fs::path settingsPath = physicalRoot / L"server" / L"data" /
        L"Settings.json";
    const json settings = readJson(settingsPath);
    const bool serviceArchive =
        manifest.value("profile", "active-source") == "service-archive";
    if (settings.at("config").at("year").get<std::string>() !=
            std::to_string(summary.year) ||
        settings.at("config").at("document_catalog_storage") != "sqlite" ||
        settings.at("config").at("server_mode") !=
            (serviceArchive ? "archive" : "active"))
    {
        throw std::runtime_error("synthetic Settings.json contract mismatch");
    }
    const json& configuredRoots = settings.at("config").at("index_roots");
    if (!configuredRoots.is_array() || configuredRoots.size() != 13)
        throw std::runtime_error("synthetic Settings index-root count mismatch");
    for (std::size_t index = 0; index < prmMonthNames.size(); ++index) {
        const fs::path name = fromUtf8(
            prmMonthNames.at(index).get<std::string>());
        const fs::path logicalDirectory = logicalRoot / L"content" / name;
        if (fromUtf8(configuredRoots.at(index).get<std::string>()) !=
                logicalDirectory ||
            !fs::is_directory(physicalRoot / L"content" / name))
        {
            throw std::runtime_error("synthetic PRM month directory mismatch");
        }
    }
    const fs::path logicalTlg = logicalRoot / L"content" / tlgName;
    if (fromUtf8(configuredRoots.at(12).get<std::string>()) != logicalTlg ||
        !fs::is_directory(physicalRoot / L"content" / tlgName))
    {
        throw std::runtime_error("synthetic TLG directory mismatch");
    }
    if (fs::exists(physicalRoot / L"content" / L"autopad"))
        throw std::runtime_error("artificial content/autopad hierarchy exists");
    for (fs::recursive_directory_iterator it(physicalRoot), end; it != end; ++it) {
        if (it->is_regular_file())
            summary.generatedBytes += it->file_size();
    }
    return summary;
}

void verifyServiceArchiveContract(
    const fs::path& physicalRoot,
    const fs::path& logicalRoot,
    const json& standManifest)
{
    if (standManifest.value("profile", "") != "service-archive")
        return;
    if (!fs::is_regular_file(
            physicalRoot / L"server" / L"program" / L"SearchEngine.exe") ||
        !fs::is_regular_file(
            physicalRoot / L"server" / L"data" / L"inverted_index.sqlite") ||
        !fs::is_regular_file(physicalRoot / L"Activate-Archived-Stand.bat") ||
        !fs::is_regular_file(physicalRoot / L"Prepare-Archived-Stand.bat") ||
        !fs::is_regular_file(
            physicalRoot / L"tools" / L"SearchEngineArchiveE2EStand.exe") ||
        !fs::is_regular_file(physicalRoot / L"README-V3-RU.txt"))
    {
        throw std::runtime_error("service archive stand is incomplete");
    }

    Database index(
        physicalRoot / L"server" / L"data" / L"inverted_index.sqlite",
        SQLITE_OPEN_READONLY);
    verifyIntegrity(index.get());
    sqlite3_stmt* docs = nullptr;
    if (sqlite3_prepare_v2(
            index.get(), "SELECT COUNT(*) FROM docs WHERE deleted=0",
            -1, &docs, nullptr) != SQLITE_OK ||
        sqlite3_step(docs) != SQLITE_ROW ||
        sqlite3_column_int(docs, 0) !=
            standManifest.at("records_per_month").get<int>() * 24)
    {
        sqlite3_finalize(docs);
        throw std::runtime_error("synthetic index document inventory mismatch");
    }
    sqlite3_finalize(docs);

    const json archive = readJson(physicalRoot / kArchiveManifestName);
    if (archive.value("operation", "") != "service-archive" ||
        archive.value("format_version", 0) != 1 ||
        archive.value("phase", "") != "archive-running-source-cleaned" ||
        archive.at("year").get<int>() != standManifest.at("year").get<int>())
    {
        throw std::runtime_error("service archive operation manifest mismatch");
    }
    const fs::path archivedExecutable = fromUtf8(
        archive.at("archived_executable").get<std::string>());
    const fs::path archivedData = fromUtf8(
        archive.at("archived_data_directory").get<std::string>());
    if (archivedExecutable !=
            logicalRoot / L"server" / L"program" / L"SearchEngine.exe" ||
        archivedData != logicalRoot / L"server" / L"data")
    {
        throw std::runtime_error("service archive logical paths mismatch");
    }
    for (const auto& mapping : archive.at("mappings")) {
        const fs::path target = fromUtf8(
            mapping.at("target").get<std::string>());
        if (!isContained(target, logicalRoot) ||
            !fs::is_directory(physicalPath(target, logicalRoot, physicalRoot)))
        {
            throw std::runtime_error("service archive mapping escapes stand");
        }
    }
    if (archive.at("monthly_databases").size() != 24)
        throw std::runtime_error("service archive has no 24 monthly databases");

    const fs::path restoreRoot = fromUtf8(
        standManifest.at("restore_root").get<std::string>());
    const json settings = readJson(
        physicalRoot / L"server" / L"data" / L"Settings.json");
    const json& config = settings.at("config");
    if (config.at("asio_port").get<int>() !=
            standManifest.at("port").get<int>() ||
        fromUtf8(config.at("tlg_send_root").get<std::string>()) !=
            restoreRoot / L"content" / L"TLG" ||
        fromUtf8(config.at("razn_output_dir").get<std::string>()) !=
            restoreRoot / L"production" / L"RAZN" ||
        fromUtf8(config.at("opis_base_dir").get<std::string>()) !=
            restoreRoot / L"production" / L"OPIS" ||
        fromUtf8(config.at("f12_base_dir").get<std::string>()) !=
            restoreRoot / L"production" / L"F12")
    {
        throw std::runtime_error(
            "legacy restore Settings path contract mismatch");
    }
}

bool samePath(const fs::path& left, const fs::path& right)
{
    return lower(absoluteNormalized(left).wstring()) ==
        lower(absoluteNormalized(right).wstring());
}

fs::path rebasePath(
    const fs::path& value,
    const fs::path& oldRoot,
    const fs::path& newRoot)
{
    const fs::path normalized = absoluteNormalized(value);
    if (samePath(oldRoot, newRoot))
        return normalized;
    if (isContained(normalized, oldRoot))
        return (newRoot / normalized.lexically_relative(oldRoot)).lexically_normal();
    if (isContained(normalized, newRoot))
        return normalized;
    throw std::runtime_error(
        "portable path is outside its declared roots: " + utf8(normalized));
}

fs::path rebaseArchiveOrRestorePath(
    const fs::path& value,
    const fs::path& oldArchiveRoot,
    const fs::path& newArchiveRoot,
    const fs::path& oldRestoreRoot,
    const fs::path& newRestoreRoot)
{
    const fs::path normalized = absoluteNormalized(value);
    if (isContained(normalized, oldArchiveRoot) ||
        isContained(normalized, newArchiveRoot))
    {
        return rebasePath(normalized, oldArchiveRoot, newArchiveRoot);
    }
    if (isContained(normalized, oldRestoreRoot) ||
        isContained(normalized, newRestoreRoot))
    {
        return rebasePath(normalized, oldRestoreRoot, newRestoreRoot);
    }
    throw std::runtime_error(
        "portable manifest path is outside archive/restore roots: " +
        utf8(normalized));
}

void writeJsonAtomically(const fs::path& path, const json& value)
{
    const fs::path temporary = path.wstring() + L".portable-prepare.tmp";
    if (fs::exists(temporary)) {
        std::error_code removeError;
        fs::remove(temporary, removeError);
        if (removeError)
            throw std::runtime_error("cannot remove stale JSON preparation file");
    }
    writeText(temporary, value.dump(2) + "\n");
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD code = GetLastError();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error(
            "cannot publish prepared JSON: " +
            std::system_category().message(static_cast<int>(code)));
    }
}

void rewriteSqlitePathColumn(
    const fs::path& databasePath,
    const char* selectSql,
    const char* updateSql,
    const fs::path& oldArchiveRoot,
    const fs::path& newArchiveRoot)
{
    Database database(databasePath, SQLITE_OPEN_READWRITE);
    sqlite3_stmt* select = nullptr;
    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), selectSql, -1, &select, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            database.get(), updateSql, -1, &update, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(select);
        sqlite3_finalize(update);
        throw std::runtime_error("cannot prepare portable SQLite path rewrite");
    }
    database.execute("BEGIN IMMEDIATE");
    try {
        while (sqlite3_step(select) == SQLITE_ROW) {
            const sqlite3_int64 id = sqlite3_column_int64(select, 0);
            const auto* raw = sqlite3_column_text(select, 1);
            if (!raw)
                throw std::runtime_error("portable SQLite path is NULL");
            const fs::path original = fromUtf8(
                reinterpret_cast<const char*>(raw));
            const fs::path prepared = rebasePath(
                original, oldArchiveRoot, newArchiveRoot);
            if (samePath(original, prepared))
                continue;
            const std::string preparedUtf8 = utf8(prepared);
            sqlite3_bind_text(
                update, 1, preparedUtf8.c_str(),
                static_cast<int>(preparedUtf8.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, id);
            if (sqlite3_step(update) != SQLITE_DONE)
                throw std::runtime_error("portable SQLite path update failed");
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
        }
        sqlite3_finalize(select);
        select = nullptr;
        sqlite3_finalize(update);
        update = nullptr;
        database.execute("COMMIT");
    } catch (...) {
        sqlite3_finalize(select);
        sqlite3_finalize(update);
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
}

void rewritePortableSettings(
    const fs::path& settingsPath,
    const fs::path& oldArchiveRoot,
    const fs::path& newArchiveRoot,
    const fs::path& oldRestoreRoot,
    const fs::path& newRestoreRoot)
{
    json settings = readJson(settingsPath);
    json& config = settings.at("config");
    const auto rewriteValue = [&](json& value) {
        if (!value.is_string())
            throw std::runtime_error("portable Settings path is not a string");
        const fs::path path = fromUtf8(value.get<std::string>());
        if (path.empty())
            return;
        value = utf8(rebaseArchiveOrRestorePath(
            path, oldArchiveRoot, newArchiveRoot,
            oldRestoreRoot, newRestoreRoot));
    };
    for (const char* name : {"index_roots", "excluded_subtrees", "exclude_dirs"}) {
        if (!config.contains(name))
            continue;
        if (!config.at(name).is_array())
            throw std::runtime_error(std::string("portable config.") + name + " is not an array");
        for (json& value : config[name])
            rewriteValue(value);
    }
    for (const char* name : {
             "prm_monthly_bases_dir", "prd_monthly_bases_dir",
             "tlg_send_root", "razn_output_dir", "opis_base_dir",
             "f12_base_dir"})
    {
        if (config.contains(name))
            rewriteValue(config[name]);
    }
    writeJsonAtomically(settingsPath, settings);
}

void refreshTargetHashes(
    json& archive,
    const fs::path& physicalRoot,
    const fs::path& logicalRoot)
{
    for (json& item : archive.at("files")) {
        const fs::path target = fromUtf8(item.at("target").get<std::string>());
        const FileHashResult hash = sha256File(
            physicalPath(target, logicalRoot, physicalRoot));
        if (!hash.ok)
            throw std::runtime_error(hash.message);
        item["target_size"] = hash.size;
        item["target_sha256"] = hash.sha256;
        item["transformed"] =
            item.value("source_size", std::uintmax_t{}) != hash.size ||
            item.value("source_sha256", std::string()) != hash.sha256;
    }
    for (json& item : archive.at("monthly_databases")) {
        const fs::path target = fromUtf8(item.at("target").get<std::string>());
        const FileHashResult hash = sha256File(
            physicalPath(target, logicalRoot, physicalRoot));
        if (!hash.ok)
            throw std::runtime_error(hash.message);
        item["target_size"] = hash.size;
        item["target_sha256"] = hash.sha256;
    }
}

void rewritePortableArchiveManifest(
    json& archive,
    const fs::path& physicalRoot,
    const fs::path& oldArchiveRoot,
    const fs::path& newArchiveRoot,
    const fs::path& oldRestoreRoot,
    const fs::path& newRestoreRoot)
{
    const auto rewriteField = [&](const char* name) {
        const fs::path value = fromUtf8(
            archive.at(name).get<std::string>());
        archive[name] = utf8(rebaseArchiveOrRestorePath(
            value, oldArchiveRoot, newArchiveRoot,
            oldRestoreRoot, newRestoreRoot));
    };
    for (const char* name : {
             "original_executable", "archived_executable",
             "original_data_directory", "archived_data_directory",
             "original_prm_monthly_directory",
             "original_prd_monthly_directory"})
    {
        rewriteField(name);
    }
    for (json& mapping : archive.at("mappings")) {
        for (const char* name : {"source", "target"}) {
            const fs::path value = fromUtf8(
                mapping.at(name).get<std::string>());
            mapping[name] = utf8(rebaseArchiveOrRestorePath(
                value, oldArchiveRoot, newArchiveRoot,
                oldRestoreRoot, newRestoreRoot));
        }
    }
    for (json& item : archive.at("files")) {
        for (const char* name : {"source", "target"}) {
            const fs::path value = fromUtf8(item.at(name).get<std::string>());
            item[name] = utf8(rebaseArchiveOrRestorePath(
                value, oldArchiveRoot, newArchiveRoot,
                oldRestoreRoot, newRestoreRoot));
        }
    }
    for (json& item : archive.at("monthly_databases")) {
        for (const char* name : {"source", "target"}) {
            const fs::path value = fromUtf8(item.at(name).get<std::string>());
            item[name] = utf8(rebaseArchiveOrRestorePath(
                value, oldArchiveRoot, newArchiveRoot,
                oldRestoreRoot, newRestoreRoot));
        }
    }

    const std::wstring serviceName = safeServiceName(
        encoding::utf8_to_wstring(
            archive.at("service_name").get<std::string>()));
    const fs::path archivedExecutable = fromUtf8(
        archive.at("archived_executable").get<std::string>());
    const fs::path archivedData = fromUtf8(
        archive.at("archived_data_directory").get<std::string>());
    const fs::path originalExecutable = fromUtf8(
        archive.at("original_executable").get<std::string>());
    const fs::path originalData = fromUtf8(
        archive.at("original_data_directory").get<std::string>());
    archive["archived_image_path"] = encoding::wstring_to_utf8(
        serviceImagePath(archivedExecutable, serviceName, archivedData));
    archive["original_image_path"] = encoding::wstring_to_utf8(
        serviceImagePath(originalExecutable, serviceName, originalData));
    archive["portable_prepared"] = true;
    refreshTargetHashes(archive, physicalRoot, newArchiveRoot);
}

struct WorkstationPathMapping {
    fs::path source;
    fs::path target;
};

fs::path mapWorkstationPath(
    const fs::path& value,
    const std::vector<WorkstationPathMapping>& mappings)
{
    const fs::path normalized = absoluteNormalized(value);
    for (const auto& mapping : mappings) {
        if (isContained(normalized, mapping.source)) {
            return (mapping.target /
                    normalized.lexically_relative(mapping.source))
                .lexically_normal();
        }
    }
    for (const auto& mapping : mappings) {
        if (isContained(normalized, mapping.target))
            return normalized;
    }
    throw std::runtime_error(
        "workstation path is outside synthetic content roots: " +
        utf8(normalized));
}

std::vector<WorkstationPathMapping> workstationContentMappings(
    const WorkstationStandLayout& layout,
    const json& stand)
{
    std::vector<WorkstationPathMapping> mappings;
    const json& names =
        stand.at("content_layout").at("prm_month_directories");
    if (!names.is_array() || names.size() != layout.monthDirectories.size())
        throw std::runtime_error("workstation month layout is incomplete");
    for (std::size_t index = 0; index < names.size(); ++index) {
        mappings.push_back({
            layout.standRoot / L"content" /
                fromUtf8(names.at(index).get<std::string>()),
            layout.monthDirectories.at(index)});
    }
    mappings.push_back({
        layout.standRoot / L"content" /
            fromUtf8(stand.at("content_layout")
                         .at("prd_tlg_directory")
                         .get<std::string>()),
        layout.tlgDirectory});
    return mappings;
}

void rewriteMappedSqlitePathColumn(
    const fs::path& databasePath,
    const char* selectSql,
    const char* updateSql,
    const std::vector<WorkstationPathMapping>& mappings)
{
    Database database(databasePath, SQLITE_OPEN_READWRITE);
    sqlite3_stmt* select = nullptr;
    sqlite3_stmt* update = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), selectSql, -1, &select, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            database.get(), updateSql, -1, &update, nullptr) != SQLITE_OK)
    {
        sqlite3_finalize(select);
        sqlite3_finalize(update);
        throw std::runtime_error(
            "cannot prepare workstation SQLite path rewrite");
    }
    database.execute("BEGIN IMMEDIATE");
    try {
        std::vector<std::pair<sqlite3_int64, fs::path>> rows;
        while (sqlite3_step(select) == SQLITE_ROW) {
            const sqlite3_int64 id = sqlite3_column_int64(select, 0);
            const auto* raw = sqlite3_column_text(select, 1);
            if (!raw)
                throw std::runtime_error("workstation SQLite path is NULL");
            rows.emplace_back(
                id, fromUtf8(reinterpret_cast<const char*>(raw)));
        }
        sqlite3_finalize(select);
        select = nullptr;
        for (const auto& row : rows) {
            const fs::path target =
                mapWorkstationPath(row.second, mappings);
            const std::string targetUtf8 = utf8(target);
            sqlite3_bind_text(
                update, 1, targetUtf8.c_str(),
                static_cast<int>(targetUtf8.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(update, 2, row.first);
            if (sqlite3_step(update) != SQLITE_DONE)
                throw std::runtime_error(
                    "workstation SQLite path update failed");
            sqlite3_reset(update);
            sqlite3_clear_bindings(update);
        }
        sqlite3_finalize(update);
        update = nullptr;
        database.execute("COMMIT");
        verifyIntegrity(database.get());
    } catch (...) {
        sqlite3_finalize(select);
        sqlite3_finalize(update);
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
}

void rewriteWorkstationSettings(
    const fs::path& settingsPath,
    const WorkstationStandLayout& layout)
{
    json settings = readJson(settingsPath);
    json& config = settings.at("config");
    config["index_roots"] = json::array();
    for (const auto& month : layout.monthDirectories)
        config["index_roots"].push_back(utf8(month));
    config["index_roots"].push_back(utf8(layout.tlgDirectory));
    config["excluded_subtrees"] =
        json::array({utf8(layout.tlgDirectory / L"OUT")});
    config["server_mode"] = "active";
    config["document_catalog_storage"] = "sqlite";
    config["scan_on_startup"] = false;
    config["prm_base_dir"] = utf8(layout.prmBaseDirectory);
    config["prd_base_dir"] = utf8(layout.prdBaseDirectory);
    config["prm_monthly_bases_dir"] =
        utf8(layout.prmMonthlyDirectory);
    config["prd_monthly_bases_dir"] =
        utf8(layout.prdMonthlyDirectory);
    config["tlg_send_root"] = utf8(layout.dataVolumeRoot);
    config["razn_output_dir"] = utf8(layout.raznDirectory);
    config["opis_base_dir"] = utf8(layout.opisDirectory);
    config["f12_base_dir"] = utf8(layout.f12Directory);
    writeJsonAtomically(settingsPath, settings);
}

void requireWorkstationSource(
    const fs::path& path,
    bool directory,
    const char* label)
{
    const bool exists = directory
        ? fs::is_directory(path)
        : fs::is_regular_file(path);
    if (!exists) {
        throw std::runtime_error(
            std::string("workstation source is missing (") + label +
            "): " + utf8(path));
    }
}

std::vector<fs::path> workstationDestinationRoots(
    const WorkstationStandLayout& layout)
{
    std::vector<fs::path> result{
        layout.installRoot,
        layout.dataDirectory,
        layout.prmMonthlyDirectory,
        layout.prdMonthlyDirectory,
        layout.tlgDirectory,
        layout.opisDirectory,
        layout.f12Directory};
    result.insert(
        result.end(),
        layout.monthDirectories.begin(),
        layout.monthDirectories.end());
    return result;
}

void validateWorkstationDestinationRoots(
    const WorkstationStandLayout& layout)
{
    const auto destinations = workstationDestinationRoots(layout);
    std::vector<std::pair<fs::path, std::string>> collisions;
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const fs::path& destination = destinations[index];
        if (isDriveRoot(destination) ||
            isContained(layout.standRoot, destination) ||
            isContained(destination, layout.standRoot))
        {
            throw std::runtime_error(
                "workstation destination overlaps the archive stand: " +
                utf8(destination));
        }
        for (std::size_t other = index + 1;
             other < destinations.size(); ++other)
        {
            if (isContained(destination, destinations[other]) ||
                isContained(destinations[other], destination))
            {
                throw std::runtime_error(
                    "workstation destination roots overlap: " +
                    utf8(destination));
            }
        }
        const WorkstationDirectoryInspection inspection =
            inspectWorkstationDirectory(destination, true);
        if (!inspection.problem.empty())
            collisions.emplace_back(destination, inspection.problem);
    }
    for (const fs::path& container : {
             layout.prmBaseDirectory, layout.prdBaseDirectory})
    {
        const WorkstationDirectoryInspection inspection =
            inspectWorkstationDirectory(container, false);
        if (!inspection.problem.empty())
            collisions.emplace_back(container, inspection.problem);
    }
    if (collisions.empty())
        return;
    std::ostringstream message;
    message <<
        "workstation deployment accepts only absent or empty destination "
        "directories:";
    for (const auto& collision : collisions)
        message << "\n  " << utf8(collision.first) << " ("
                << collision.second << ')';
    throw std::runtime_error(message.str());
}

struct WorkstationDeploymentJournal {
    std::vector<fs::path> createdFiles;
    std::vector<fs::path> createdDirectories;
};

void ensureWorkstationDirectory(
    const fs::path& path,
    bool requireEmpty,
    WorkstationDeploymentJournal& journal)
{
    WorkstationDirectoryInspection inspection =
        inspectWorkstationDirectory(path, requireEmpty);
    if (!inspection.problem.empty()) {
        throw std::runtime_error(
            "unsafe workstation destination: " + utf8(path) + " (" +
            inspection.problem + ')');
    }
    if (inspection.exists)
        return;

    std::error_code error;
    if (fs::create_directory(path, error)) {
        journal.createdDirectories.push_back(path);
        return;
    }
    if (error) {
        throw std::runtime_error(
            "cannot create workstation destination: " + utf8(path) +
            " (" + error.message() + ')');
    }

    // A directory may appear after preflight. Adopt it only if it still
    // satisfies the same empty/real-directory contract.
    inspection = inspectWorkstationDirectory(path, requireEmpty);
    if (!inspection.exists || !inspection.problem.empty()) {
        throw std::runtime_error(
            "workstation destination appeared during deployment: " +
            utf8(path));
    }
}

void copyWorkstationFile(
    const fs::path& source,
    const fs::path& destination,
    WorkstationDeploymentJournal& journal)
{
    std::error_code error;
    if (!fs::copy_file(
            source, destination, fs::copy_options::none, error))
    {
        throw std::runtime_error(
            "cannot copy workstation file without replacement: " +
            utf8(destination) +
            (error ? " (" + error.message() + ')' : std::string()));
    }
    journal.createdFiles.push_back(destination);
}

void copyWorkstationDirectoryTree(
    const fs::path& source,
    const fs::path& destination,
    WorkstationDeploymentJournal& journal)
{
    if (!fs::is_directory(source)) {
        throw std::runtime_error(
            "workstation source is not a directory: " + utf8(source));
    }
    ensureWorkstationDirectory(destination, false, journal);
    for (fs::recursive_directory_iterator it(source), end; it != end; ++it) {
        const fs::path relative = it->path().lexically_relative(source);
        const fs::path target = destination / relative;
        if (it->is_symlink()) {
            throw std::runtime_error(
                "workstation source contains a link: " + utf8(it->path()));
        }
        if (it->is_directory()) {
            ensureWorkstationDirectory(target, false, journal);
        } else if (it->is_regular_file()) {
            ensureWorkstationDirectory(target.parent_path(), false, journal);
            copyWorkstationFile(it->path(), target, journal);
        } else {
            throw std::runtime_error(
                "workstation source contains an unsupported entry: " +
                utf8(it->path()));
        }
    }
}

void rollbackWorkstationDeployment(
    const WorkstationDeploymentJournal& journal) noexcept
{
    for (auto it = journal.createdFiles.rbegin();
         it != journal.createdFiles.rend(); ++it)
    {
        std::error_code ignored;
        fs::remove(*it, ignored);
    }
    for (auto it = journal.createdDirectories.rbegin();
         it != journal.createdDirectories.rend(); ++it)
    {
        std::error_code ignored;
        // Remove only directories created by this deployment and only while
        // they are empty. Pre-existing directories are never recorded here.
        fs::remove(*it, ignored);
    }
}

void verifyMappedSqlitePaths(
    const fs::path& databasePath,
    const char* selectSql,
    const std::vector<fs::path>& allowedRoots)
{
    Database database(databasePath, SQLITE_OPEN_READONLY);
    database.execute("PRAGMA query_only=ON");
    verifyIntegrity(database.get());
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database.get(), selectSql, -1, &statement, nullptr) != SQLITE_OK)
    {
        throw std::runtime_error(
            "cannot verify workstation SQLite paths");
    }
    try {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* raw = sqlite3_column_text(statement, 0);
            if (!raw)
                throw std::runtime_error(
                    "workstation verification found a NULL path");
            const fs::path value = fromUtf8(
                reinterpret_cast<const char*>(raw));
            const bool allowed = std::any_of(
                allowedRoots.begin(), allowedRoots.end(),
                [&](const fs::path& root) {
                    return isContained(value, root);
                });
            if (!allowed) {
                throw std::runtime_error(
                    "workstation verification found an escaped path: " +
                    utf8(value));
            }
        }
        sqlite3_finalize(statement);
    } catch (...) {
        sqlite3_finalize(statement);
        throw;
    }
}

} // namespace

StandSummary generateStand(const StandOptions& options)
{
    if (options.year < 2000 || options.year > 2099)
        throw std::invalid_argument("year must be inside 2000..2099");
    if (options.recordsPerMonth < 10 || options.recordsPerMonth > 100)
        throw std::invalid_argument("records per month must be inside 10..100");
    if (!fs::is_regular_file(options.settingsTemplate))
        throw std::invalid_argument("Settings template is missing");
    const ContentLayout contentLayout =
        contentLayoutFromSettings(options.settingsTemplate);

    const fs::path logicalRoot = absoluteNormalized(options.root);
    if (isDriveRoot(logicalRoot) || logicalRoot.parent_path().empty())
        throw std::invalid_argument("stand root must not be a drive root");
    if (fs::exists(logicalRoot))
        throw std::invalid_argument("stand root already exists");

    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const fs::path staging = logicalRoot.parent_path() /
        (L"." + logicalRoot.filename().wstring() + L".staging-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(stamp));
    fs::create_directories(staging);
    writeText(staging / kMarkerName, "SearchEngine archive E2E stand\r\n");

    json manifest{
        {"format_version", 2},
        {"kind", "searchengine-archive-e2e-stand"},
        {"root", utf8(logicalRoot)},
        {"year", options.year},
        {"records_per_month", options.recordsPerMonth},
        {"schema_reference", json::array({
            "D:/BASES/ARCHIVE.db3 (structure only)",
            "D:/BASES_PRD/ARCHIVE.db3 (structure only)",
            "D:/BASES_PRD/METH_BASES/02-2026.db3 (structure only)"})},
        {"content_layout", {
            {"prm_month_directories", json::array()},
            {"prd_tlg_directory", utf8(contentLayout.tlgName)}}},
        {"f12", {
            {"relative_path", utf8(f12DatabaseRelativePath(options.year))},
            {"schema_version", kF12WaySchemaVersion},
            {"rows", options.recordsPerMonth * 24},
            {"prm_type", 1},
            {"prd_type", 2}}},
        {"databases", json::array()}};
    for (const auto& monthName : contentLayout.prmMonthNames) {
        manifest["content_layout"]["prm_month_directories"].push_back(
            utf8(monthName));
    }
    try {
        createSettings(options, logicalRoot, staging, contentLayout);
        std::vector<Row> indexedRows;
        for (const bool prm : {true, false}) {
            const fs::path baseRelative = fs::path(L"autopad") /
                (prm ? L"PRM" : L"PRD");
            for (int month = 1; month <= 12; ++month) {
                const fs::path contentRelative = fs::path(L"content") /
                    (prm
                        ? contentLayout.prmMonthNames.at(month - 1)
                        : contentLayout.tlgName);
                auto rows = makeRows(
                    prm, options.year, month, options.recordsPerMonth,
                    logicalRoot, staging, contentRelative);
                indexedRows.insert(
                    indexedRows.end(), rows.begin(), rows.end());
                const fs::path relative = baseRelative / L"METH_BASES" /
                    (encoding::utf8_to_wstring(monthText(month)) + L"-" +
                     std::to_wstring(options.year) + L".db3");
                createDatabase(staging / relative, prm, rows);
                manifest["databases"].push_back({
                    {"source", sourceName(prm)},
                    {"month", month},
                    {"relative_path", utf8(relative)},
                    {"rows", options.recordsPerMonth}});
            }
            // ARCHIVE.db3 is deliberately absent. It is a live operational
            // database, not a year/month archive. The stand therefore also
            // proves that read-only server requests do not recreate it.
        }
        createF12Database(
            staging / f12DatabaseRelativePath(options.year),
            options.year, indexedRows);
        writeJson(staging / kManifestName, manifest);
        StandSummary summary = verifyInternal(staging, logicalRoot, manifest);
        for (const fs::path& liveArchive : {
                 staging / L"autopad" / L"PRM" / L"ARCHIVE.db3",
                 staging / L"autopad" / L"PRD" / L"ARCHIVE.db3"}) {
            if (fs::exists(liveArchive))
                throw std::runtime_error("live ARCHIVE.db3 must not be generated");
        }
        std::error_code publishError;
        fs::rename(staging, logicalRoot, publishError);
        if (publishError)
            throw std::runtime_error("cannot publish stand: " + publishError.message());
        summary.root = logicalRoot;
        return summary;
    } catch (...) {
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
        throw;
    }
}

StandSummary generateServiceArchiveStand(
    const ServiceArchiveStandOptions& options)
{
    if (options.stand.year < 2000 || options.stand.year > 2099)
        throw std::invalid_argument("year must be inside 2000..2099");
    if (options.stand.recordsPerMonth < 10 ||
        options.stand.recordsPerMonth > 100)
    {
        throw std::invalid_argument("records per month must be inside 10..100");
    }
    if (options.port < 1 || options.port > 65535)
        throw std::invalid_argument("port must be inside 1..65535");
    if (!fs::is_regular_file(options.stand.settingsTemplate))
        throw std::invalid_argument("Settings template is missing");
    if (!fs::is_directory(options.programTemplate) ||
        !fs::is_regular_file(options.programTemplate / L"SearchEngine.exe"))
    {
        throw std::invalid_argument(
            "program template must contain SearchEngine.exe");
    }
    if (!fs::is_regular_file(options.preparerTemplate))
        throw std::invalid_argument("portable preparer template is missing");
    if (!options.installerTemplate.empty() &&
        (!fs::is_directory(options.installerTemplate) ||
         !fs::is_regular_file(
             options.installerTemplate / L"tools" /
             L"SearchEngineConfig.exe") ||
         !fs::is_regular_file(
             options.installerTemplate / L"prerequisites" /
             L"vc_redist.x86.exe")))
    {
        throw std::invalid_argument(
            "installer template is not a complete x86 package");
    }

    const fs::path physicalRoot = absoluteNormalized(options.stand.root);
    const fs::path logicalRoot = absoluteNormalized(options.deploymentRoot);
    const fs::path restoreRoot = absoluteNormalized(options.restoreRoot);
    const std::wstring serviceName = safeServiceName(options.serviceName);
    const fs::path expectedLeaf =
        serviceName + L"-" + std::to_wstring(options.stand.year);
    if (isDriveRoot(physicalRoot) || isDriveRoot(logicalRoot) ||
        isDriveRoot(restoreRoot) || physicalRoot.parent_path().empty() ||
        logicalRoot.filename() != expectedLeaf ||
        physicalRoot.filename() != expectedLeaf)
    {
        throw std::invalid_argument(
            "service archive root must use <service-name>-<year> as its leaf");
    }
    if (isContained(restoreRoot, logicalRoot) ||
        isContained(logicalRoot, restoreRoot))
    {
        throw std::invalid_argument(
            "restore root must be outside the archive stand");
    }
    if (fs::exists(physicalRoot))
        throw std::invalid_argument("stand root already exists");

    const ContentLayout contentLayout =
        contentLayoutFromSettings(options.stand.settingsTemplate);
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const fs::path staging = physicalRoot.parent_path() /
        (L"." + physicalRoot.filename().wstring() + L".staging-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(stamp));
    fs::create_directories(staging);
    writeText(staging / kMarkerName, "SearchEngine archive E2E stand v3\r\n");

    json manifest{
        {"format_version", 3},
        {"kind", "searchengine-archive-e2e-stand"},
        {"profile", "service-archive"},
        {"root", utf8(logicalRoot)},
        {"restore_root", utf8(restoreRoot)},
        {"service_name", encoding::wstring_to_utf8(serviceName)},
        {"port", options.port},
        {"year", options.stand.year},
        {"records_per_month", options.stand.recordsPerMonth},
        {"schema_reference", json::array({
            "D:/BASES/ARCHIVE.db3 (structure only)",
            "D:/BASES_PRD/ARCHIVE.db3 (structure only)",
            "D:/BASES_PRD/METH_BASES/02-2026.db3 (structure only)"})},
        {"content_layout", {
            {"prm_month_directories", json::array()},
            {"prd_tlg_directory", utf8(contentLayout.tlgName)}}},
        {"f12", {
            {"relative_path", utf8(f12DatabaseRelativePath(options.stand.year))},
            {"schema_version", kF12WaySchemaVersion},
            {"rows", options.stand.recordsPerMonth * 24},
            {"prm_type", 1},
            {"prd_type", 2}}},
        {"databases", json::array()}};
    for (const auto& monthName : contentLayout.prmMonthNames) {
        manifest["content_layout"]["prm_month_directories"].push_back(
            utf8(monthName));
    }
    if (!options.installerTemplate.empty())
        manifest["workstation_mode_available"] = true;

    try {
        createSettings(options.stand, logicalRoot, staging, contentLayout);
        freezeServiceArchiveSettings(
            staging, logicalRoot, restoreRoot, options.port);
        copyDirectoryTree(
            options.programTemplate,
            staging / L"server" / L"program");
        fs::create_directories(staging / L"tools");
        fs::copy_file(
            options.preparerTemplate,
            staging / L"tools" / L"SearchEngineArchiveE2EStand.exe",
            fs::copy_options::none);
        if (!options.installerTemplate.empty()) {
            copyDirectoryTree(
                options.installerTemplate,
                staging / L"installer");
        }
        for (const wchar_t* file : {L"ignore.txt", L"prefix_map.json", L"OEM866.INI"}) {
            const fs::path source = options.stand.settingsTemplate.parent_path() / file;
            if (fs::is_regular_file(source)) {
                fs::copy_file(
                    source,
                    staging / L"server" / L"data" / file,
                    fs::copy_options::overwrite_existing);
            }
        }

        std::vector<Row> indexedRows;
        for (const bool prm : {true, false}) {
            const fs::path baseRelative = fs::path(L"autopad") /
                (prm ? L"PRM" : L"PRD");
            for (int month = 1; month <= 12; ++month) {
                const fs::path contentRelative = fs::path(L"content") /
                    (prm
                        ? contentLayout.prmMonthNames.at(month - 1)
                        : contentLayout.tlgName);
                auto rows = makeRows(
                    prm, options.stand.year, month,
                    options.stand.recordsPerMonth,
                    logicalRoot, staging, contentRelative);
                indexedRows.insert(
                    indexedRows.end(), rows.begin(), rows.end());
                const fs::path relative = baseRelative / L"METH_BASES" /
                    (encoding::utf8_to_wstring(monthText(month)) + L"-" +
                     std::to_wstring(options.stand.year) + L".db3");
                createDatabase(staging / relative, prm, rows);
                manifest["databases"].push_back({
                    {"source", sourceName(prm)},
                    {"month", month},
                    {"relative_path", utf8(relative)},
                    {"rows", options.stand.recordsPerMonth}});
            }
        }
        createF12Database(
            staging / f12DatabaseRelativePath(options.stand.year),
            options.stand.year, indexedRows);
        createSyntheticIndex(
            staging / L"server" / L"data" / L"inverted_index.sqlite",
            logicalRoot, staging, indexedRows);
        writePreparationScript(staging);
        writeActivationScript(staging, serviceName);
        if (!options.installerTemplate.empty()) {
            writeWorkstationDeploymentScript(
                staging, serviceName, options.port, options.stand.year);
            writeWorkstationReadme(staging, serviceName);
        }
        writeServiceArchiveReadme(
            staging, logicalRoot, restoreRoot, serviceName, options.port);
        writeServiceArchiveManifest(
            options, staging, logicalRoot, restoreRoot, manifest);
        writeJson(staging / kManifestName, manifest);

        StandSummary summary = verifyInternal(staging, logicalRoot, manifest);
        verifyServiceArchiveContract(staging, logicalRoot, manifest);
        std::error_code publishError;
        fs::rename(staging, physicalRoot, publishError);
        if (publishError)
            throw std::runtime_error("cannot publish stand: " + publishError.message());
        summary.root = logicalRoot;
        return summary;
    } catch (...) {
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
        throw;
    }
}

StandSummary prepareServiceArchiveStand(const fs::path& root)
{
    const fs::path physicalRoot = absoluteNormalized(root);
    if (!fs::is_directory(physicalRoot) || isDriveRoot(physicalRoot) ||
        !fs::is_regular_file(physicalRoot / kMarkerName) ||
        !fs::is_regular_file(physicalRoot / kManifestName) ||
        !fs::is_regular_file(physicalRoot / kArchiveManifestName))
    {
        throw std::invalid_argument("directory is not a portable service stand");
    }

    json stand = readJson(physicalRoot / kManifestName);
    if (stand.value("format_version", 0) != 3 ||
        stand.value("kind", "") != "searchengine-archive-e2e-stand" ||
        stand.value("profile", "") != "service-archive")
    {
        throw std::runtime_error("stand is not a portable service archive v3");
    }
    const std::wstring serviceName = safeServiceName(
        encoding::utf8_to_wstring(
            stand.at("service_name").get<std::string>()));
    const fs::path expectedLeaf =
        serviceName + L"-" + std::to_wstring(stand.at("year").get<int>());
    if (physicalRoot.filename() != expectedLeaf) {
        throw std::runtime_error(
            "portable stand directory name must remain <service-name>-<year>");
    }

    const fs::path oldArchiveRoot = absoluteNormalized(
        fromUtf8(stand.at("root").get<std::string>()));
    const fs::path oldRestoreRoot = absoluteNormalized(
        fromUtf8(stand.at("restore_root").get<std::string>()));
    if (oldRestoreRoot.filename().empty())
        throw std::runtime_error("portable restore root has no safe leaf");
    const fs::path newArchiveRoot = physicalRoot;
    const fs::path newRestoreRoot = absoluteNormalized(
        physicalRoot.parent_path() / oldRestoreRoot.filename());
    if (isContained(newRestoreRoot, newArchiveRoot) ||
        isContained(newArchiveRoot, newRestoreRoot))
    {
        throw std::runtime_error("portable restore root overlaps archive stand");
    }

    json archive = readJson(physicalRoot / kArchiveManifestName);
    if (archive.value("operation", "") != "service-archive" ||
        archive.value("phase", "") != "archive-running-source-cleaned")
    {
        throw std::runtime_error(
            "portable preparation requires a source-cleaned service archive");
    }

    if (samePath(oldArchiveRoot, newArchiveRoot) &&
        samePath(oldRestoreRoot, newRestoreRoot))
    {
        return verifyStand(physicalRoot);
    }

    rewritePortableSettings(
        physicalRoot / L"server" / L"data" / L"Settings.json",
        oldArchiveRoot, newArchiveRoot, oldRestoreRoot, newRestoreRoot);
    rewriteSqlitePathColumn(
        physicalRoot / L"server" / L"data" / L"inverted_index.sqlite",
        "SELECT doc_id,path FROM docs",
        "UPDATE docs SET path=? WHERE doc_id=?",
        oldArchiveRoot, newArchiveRoot);
    for (const json& item : stand.at("databases")) {
        const fs::path relative = fromUtf8(
            item.at("relative_path").get<std::string>());
        rewriteSqlitePathColumn(
            physicalRoot / relative,
            "SELECT \"Index\",DirectTo FROM ARCHIVE",
            "UPDATE ARCHIVE SET DirectTo=? WHERE \"Index\"=?",
            oldArchiveRoot, newArchiveRoot);
    }

    rewritePortableArchiveManifest(
        archive, physicalRoot,
        oldArchiveRoot, newArchiveRoot,
        oldRestoreRoot, newRestoreRoot);
    writeJsonAtomically(physicalRoot / kArchiveManifestName, archive);

    stand["root"] = utf8(newArchiveRoot);
    stand["restore_root"] = utf8(newRestoreRoot);
    stand["portable_prepared"] = true;
    writeJsonAtomically(physicalRoot / kManifestName, stand);
    return verifyStand(physicalRoot);
}

WorkstationStandLayout planWorkstationStandDeployment(
    const WorkstationStandOptions& options)
{
    WorkstationStandLayout layout;
    layout.standRoot = absoluteNormalized(options.root);
    layout.dataVolumeRoot = absoluteNormalized(options.dataVolumeRoot);
    const fs::path programFilesRoot =
        absoluteNormalized(options.programFilesRoot);
    const fs::path programDataRoot =
        absoluteNormalized(options.programDataRoot);
    const WorkstationDirectoryInspection dataVolumeInspection =
        inspectWorkstationDirectory(layout.dataVolumeRoot, false);
    const WorkstationDirectoryInspection programFilesInspection =
        inspectWorkstationDirectory(programFilesRoot, false);
    const WorkstationDirectoryInspection programDataInspection =
        inspectWorkstationDirectory(programDataRoot, false);
    if (!fs::is_directory(layout.standRoot) ||
        !fs::is_directory(layout.dataVolumeRoot) ||
        !fs::is_directory(programFilesRoot) ||
        !fs::is_directory(programDataRoot) ||
        !dataVolumeInspection.problem.empty() ||
        !programFilesInspection.problem.empty() ||
        !programDataInspection.problem.empty() ||
        !fs::is_regular_file(layout.standRoot / kMarkerName) ||
        !fs::is_regular_file(layout.standRoot / kManifestName) ||
        !fs::is_regular_file(layout.standRoot / kArchiveManifestName))
    {
        throw std::invalid_argument(
            "workstation deployment roots or stand are incomplete");
    }

    const json stand = readJson(layout.standRoot / kManifestName);
    if (stand.value("format_version", 0) != 3 ||
        stand.value("profile", "") != "service-archive" ||
        !stand.value("workstation_mode_available", false))
    {
        throw std::runtime_error(
            "stand has no clean-VM workstation deployment mode");
    }
    if (!samePath(
            fromUtf8(stand.at("root").get<std::string>()),
            layout.standRoot))
    {
        throw std::runtime_error(
            "portable stand must be prepared before workstation deployment");
    }

    layout.serviceName = safeServiceName(
        encoding::utf8_to_wstring(
            stand.at("service_name").get<std::string>()));
    layout.port = stand.at("port").get<int>();
    layout.year = stand.at("year").get<int>();
    const fs::path expectedLeaf =
        layout.serviceName + L"-" + std::to_wstring(layout.year);
    if (layout.standRoot.filename() != expectedLeaf)
        throw std::runtime_error(
            "workstation stand directory name does not match its service");

    layout.installRoot = programFilesRoot / layout.serviceName;
    layout.binDirectory = layout.installRoot / L"bin";
    layout.toolsDirectory = layout.installRoot / L"tools";
    layout.dataDirectory = programDataRoot / layout.serviceName;
    layout.prmBaseDirectory = layout.dataVolumeRoot / L"BASES";
    layout.prdBaseDirectory = layout.dataVolumeRoot / L"BASES_PRD";
    layout.prmMonthlyDirectory =
        layout.prmBaseDirectory / L"METH_BASES";
    layout.prdMonthlyDirectory =
        layout.prdBaseDirectory / L"METH_BASES";
    layout.tlgDirectory = layout.dataVolumeRoot /
        fromUtf8(stand.at("content_layout")
                     .at("prd_tlg_directory")
                     .get<std::string>());
    layout.opisDirectory = layout.dataVolumeRoot / L"OPIS_ADMIN";
    layout.raznDirectory =
        layout.opisDirectory / L"РАЗНОСКА_ДЛЯ_ПРОСТАВЛЕНИЯ";
    layout.f12Directory = layout.dataVolumeRoot / L"F12";

    const json& monthNames =
        stand.at("content_layout").at("prm_month_directories");
    if (!monthNames.is_array() || monthNames.size() != 12)
        throw std::runtime_error(
            "workstation stand has no twelve month directories");
    for (const auto& name : monthNames) {
        layout.monthDirectories.push_back(
            layout.dataVolumeRoot /
            fromUtf8(name.get<std::string>()));
    }

    requireWorkstationSource(
        layout.standRoot / L"server" / L"program" /
            L"SearchEngine.exe",
        false, "SearchEngine.exe");
    requireWorkstationSource(
        layout.standRoot / L"server" / L"data" / L"Settings.json",
        false, "Settings.json");
    requireWorkstationSource(
        layout.standRoot / L"server" / L"data" /
            L"inverted_index.sqlite",
        false, "inverted_index.sqlite");
    requireWorkstationSource(
        layout.standRoot / L"installer" / L"tools" /
            L"SearchEngineConfig.exe",
        false, "installer tools");
    requireWorkstationSource(
        layout.standRoot / L"installer" / L"prerequisites" /
            L"vc_redist.x86.exe",
        false, "VC redistributable");
    requireWorkstationSource(
        layout.standRoot / L"production" / L"OPIS",
        true, "OPIS");
    requireWorkstationSource(
        layout.standRoot / L"production" / L"RAZN",
        true, "RAZN");
    requireWorkstationSource(
        layout.standRoot / L"production" / L"F12",
        true, "F12");
    if (stand.contains("f12")) {
        requireWorkstationSource(
            layout.standRoot /
                fromUtf8(stand.at("f12").at("relative_path")
                             .get<std::string>()),
            false, "F12 yearly database");
    }
    for (const auto& mapping : workstationContentMappings(layout, stand))
        requireWorkstationSource(mapping.source, true, "indexed content");
    for (const auto& item : stand.at("databases")) {
        requireWorkstationSource(
            layout.standRoot /
                fromUtf8(item.at("relative_path").get<std::string>()),
            false, "monthly database");
    }

    validateWorkstationDestinationRoots(layout);
    return layout;
}

WorkstationStandLayout deployWorkstationStand(
    const WorkstationStandOptions& options)
{
    (void)prepareServiceArchiveStand(options.root);
    WorkstationStandLayout layout =
        planWorkstationStandDeployment(options);
    const json stand = readJson(layout.standRoot / kManifestName);
    const auto mappings = workstationContentMappings(layout, stand);
    WorkstationDeploymentJournal journal;
    journal.createdFiles.reserve(1024);
    journal.createdDirectories.reserve(256);

    try {
        ensureWorkstationDirectory(layout.installRoot, true, journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"server" / L"program",
            layout.binDirectory,
            journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"installer" / L"tools",
            layout.toolsDirectory,
            journal);

        ensureWorkstationDirectory(layout.dataDirectory, true, journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"server" / L"data",
            layout.dataDirectory,
            journal);
        ensureWorkstationDirectory(
            layout.dataDirectory / L"logs", false, journal);

        for (const auto& mapping : mappings) {
            ensureWorkstationDirectory(mapping.target, true, journal);
            copyWorkstationDirectoryTree(
                mapping.source, mapping.target, journal);
        }

        ensureWorkstationDirectory(
            layout.prmBaseDirectory, false, journal);
        ensureWorkstationDirectory(
            layout.prmMonthlyDirectory, true, journal);
        ensureWorkstationDirectory(
            layout.prdBaseDirectory, false, journal);
        ensureWorkstationDirectory(
            layout.prdMonthlyDirectory, true, journal);
        std::vector<fs::path> deployedDatabases;
        for (const auto& item : stand.at("databases")) {
            const fs::path source = layout.standRoot /
                fromUtf8(item.at("relative_path").get<std::string>());
            const std::string kind = item.at("source").get<std::string>();
            const fs::path destination =
                (kind == "PRM"
                     ? layout.prmMonthlyDirectory
                     : layout.prdMonthlyDirectory) /
                source.filename();
            copyWorkstationFile(source, destination, journal);
            deployedDatabases.push_back(destination);
        }

        ensureWorkstationDirectory(layout.opisDirectory, true, journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"production" / L"OPIS",
            layout.opisDirectory,
            journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"production" / L"RAZN",
            layout.raznDirectory,
            journal);
        ensureWorkstationDirectory(layout.f12Directory, true, journal);
        copyWorkstationDirectoryTree(
            layout.standRoot / L"production" / L"F12",
            layout.f12Directory,
            journal);
        const fs::path deployedF12 = layout.f12Directory /
            (std::to_wstring(layout.year) + L".db");
        const fs::path sourceF12 = layout.standRoot /
            fromUtf8(stand.at("f12").at("relative_path")
                         .get<std::string>());
        const FileHashResult sourceF12Hash = sha256File(sourceF12);
        const FileHashResult deployedF12Hash = sha256File(deployedF12);
        if (!sourceF12Hash.ok || !deployedF12Hash.ok ||
            sourceF12Hash.size != deployedF12Hash.size ||
            sourceF12Hash.sha256 != deployedF12Hash.sha256)
        {
            throw std::runtime_error(
                "deployed F12 yearly database copy mismatch");
        }
        {
            Database f12(deployedF12, SQLITE_OPEN_READONLY);
            f12.execute("PRAGMA query_only=ON");
            verifyIntegrity(f12.get());
        }

        rewriteWorkstationSettings(
            layout.dataDirectory / L"Settings.json", layout);
        rewriteMappedSqlitePathColumn(
            layout.dataDirectory / L"inverted_index.sqlite",
            "SELECT doc_id,path FROM docs",
            "UPDATE docs SET path=? WHERE doc_id=?",
            mappings);
        for (const auto& database : deployedDatabases) {
            rewriteMappedSqlitePathColumn(
                database,
                "SELECT \"Index\",DirectTo FROM ARCHIVE",
                "UPDATE ARCHIVE SET DirectTo=? WHERE \"Index\"=?",
                mappings);
        }

        std::vector<fs::path> contentRoots = layout.monthDirectories;
        contentRoots.push_back(layout.tlgDirectory);
        verifyMappedSqlitePaths(
            layout.dataDirectory / L"inverted_index.sqlite",
            "SELECT path FROM docs",
            contentRoots);
        for (const auto& database : deployedDatabases) {
            verifyMappedSqlitePaths(
                database,
                "SELECT DirectTo FROM ARCHIVE",
                contentRoots);
        }
        const json settings =
            readJson(layout.dataDirectory / L"Settings.json");
        const json& config = settings.at("config");
        if (config.at("index_roots").size() != 13 ||
            fromUtf8(config.at("tlg_send_root").get<std::string>()) !=
                layout.dataVolumeRoot ||
            fromUtf8(config.at("prm_monthly_bases_dir")
                         .get<std::string>()) !=
                layout.prmMonthlyDirectory ||
            fromUtf8(config.at("prd_monthly_bases_dir")
                         .get<std::string>()) !=
                layout.prdMonthlyDirectory ||
            config.at("server_mode") != "active" ||
            config.at("document_catalog_storage") != "sqlite")
        {
            throw std::runtime_error(
                "deployed workstation Settings verification failed");
        }

        const json receipt{
            {"format_version", 1},
            {"kind", "searchengine-workstation-stand-deployment"},
            {"service_name", encoding::wstring_to_utf8(layout.serviceName)},
            {"year", layout.year},
            {"port", layout.port},
            {"program_directory", utf8(layout.binDirectory)},
            {"tools_directory", utf8(layout.toolsDirectory)},
            {"data_directory", utf8(layout.dataDirectory)},
            {"data_volume_root", utf8(layout.dataVolumeRoot)},
            {"prm_monthly_directory", utf8(layout.prmMonthlyDirectory)},
            {"prd_monthly_directory", utf8(layout.prdMonthlyDirectory)},
            {"tlg_directory", utf8(layout.tlgDirectory)},
            {"f12_database", utf8(deployedF12)},
            {"f12_way_rows", stand.at("f12").at("rows")}};
        writeJsonAtomically(
            layout.standRoot / L"workstation-deployment.json",
            receipt);
        return layout;
    } catch (...) {
        rollbackWorkstationDeployment(journal);
        throw;
    }
}

StandSummary verifyStand(const fs::path& root)
{
    const fs::path physicalRoot = absoluteNormalized(root);
    if (!fs::is_directory(physicalRoot) || isDriveRoot(physicalRoot) ||
        !fs::is_regular_file(physicalRoot / kMarkerName) ||
        !fs::is_regular_file(physicalRoot / kManifestName))
    {
        throw std::invalid_argument("directory is not a marked E2E stand");
    }
    const json manifest = readJson(physicalRoot / kManifestName);
    const int formatVersion = manifest.value("format_version", 0);
    if (manifest.value("kind", "") != "searchengine-archive-e2e-stand" ||
        (formatVersion != 2 && formatVersion != 3))
    {
        throw std::runtime_error("unsupported E2E stand manifest");
    }
    const fs::path logicalRoot = fromUtf8(manifest.at("root").get<std::string>());
    if (formatVersion == 2 && absoluteNormalized(logicalRoot) != physicalRoot)
        throw std::runtime_error("E2E stand was moved; regenerate paths first");
    StandSummary summary = verifyInternal(physicalRoot, logicalRoot, manifest);
    verifyServiceArchiveContract(physicalRoot, logicalRoot, manifest);
    return summary;
}

} // namespace searchengine_archive_e2e
