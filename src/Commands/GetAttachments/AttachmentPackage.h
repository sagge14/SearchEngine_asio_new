#ifndef ATTACHMENT_PACKAGE_H
#define ATTACHMENT_PACKAGE_H

#include <filesystem>
#include <map>
#include <string>
#include <cstdint>
#include <vector>

#include <boost/serialization/map.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "boost/archive/portable_binary_iarchive.hpp"
#include "boost/archive/portable_binary_oarchive.hpp"

struct AttachmentPackage
{
    std::map<std::filesystem::path, std::vector<std::uint8_t>> attachments;

    void addAttachment(const std::filesystem::path& file_dir);
    void saveToDirectory(const std::filesystem::path& dirPath) const;

    AttachmentPackage() = default;

    [[nodiscard]] size_t getAttachCount() const { return attachments.size(); }
    static AttachmentPackage loadFromDirectory(const std::filesystem::path& dirPath);

    friend class boost::serialization::access;

    template<class Archive>
    void serialize(Archive& ar, const unsigned int)
    {
        ar & attachments;
    }

    static std::vector<uint8_t> serializeToBytes(const AttachmentPackage& fileData);
    static AttachmentPackage deserializeFromBytes(const std::vector<uint8_t>& bytes);
};

#endif
