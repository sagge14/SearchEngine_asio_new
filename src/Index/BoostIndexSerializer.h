#pragma once

#include "IIndexSerializer.h"

#include <string>

namespace inverted_index {

class BoostIndexSerializer final : public IIndexSerializer {
public:
    explicit BoostIndexSerializer(std::string path);

    [[nodiscard]] std::string kind() const override;
    [[nodiscard]] bool exists() const override;

    void save(const InvertedIndex& idx) override;
    void load(InvertedIndex& idx) override;

private:
    std::string path_;
};

} // namespace inverted_index

