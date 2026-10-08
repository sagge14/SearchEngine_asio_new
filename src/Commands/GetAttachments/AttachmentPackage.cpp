#include "AttachmentPackage.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace boost::serialization {

template<class Archive>
void serialize(Archive& ar, std::filesystem::path& p, const unsigned int)
{
    std::string str = p.string();
    ar & str;
    if (Archive::is_loading::value) {
        p = std::filesystem::path(str);
    }
}

} // namespace boost::serialization

void AttachmentPackage::addAttachment(const std::filesystem::path& file_dir)
{
    std::ifstream file(file_dir, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file: " + file_dir.string());
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
    attachments[file_dir.filename().string()] = data;
}

void AttachmentPackage::saveToDirectory(const std::filesystem::path& dirPath) const
{
    std::filesystem::create_directory(dirPath);

    for (const auto& [relative_path, data] : attachments) {
        std::filesystem::path full_path = dirPath / std::filesystem::path(relative_path);
        std::filesystem::create_directories(full_path.parent_path());

        std::ofstream file(full_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error(
                "Failed to open attachment file for writing: " + relative_path.string());
        }

        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
    }
}

AttachmentPackage AttachmentPackage::loadFromDirectory(const std::filesystem::path& dirPath)
{
    AttachmentPackage package;
    for (auto& entry : std::filesystem::recursive_directory_iterator(dirPath)) {
        if (std::filesystem::is_regular_file(entry)) {
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) {
                throw std::runtime_error(
                    "Failed to open attachment file: " + entry.path().string());
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
            std::filesystem::path relative_path =
                std::filesystem::relative(entry.path(), dirPath);
            package.attachments[relative_path.string()] = data;
        }
    }

    return package;
}

std::vector<uint8_t> AttachmentPackage::serializeToBytes(
    const AttachmentPackage& fileData)
{
    std::ostringstream oss(std::ios::binary);
    portable_binary_oarchive oa(oss);
    oa << fileData;
    const std::string& str = oss.str();

    std::vector<std::uint8_t> buffer;
    buffer.reserve(str.size());
    for (char item : str) {
        buffer.push_back(static_cast<std::uint8_t>(item));
    }
    return buffer;
}

AttachmentPackage AttachmentPackage::deserializeFromBytes(
    const std::vector<uint8_t>& bytes)
{
    std::string str(bytes.begin(), bytes.end());
    std::istringstream iss(str, std::ios::binary);
    portable_binary_iarchive ia(iss);

    AttachmentPackage package;
    ia >> package;
    return package;
}
