#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

#include "robin_hood.h"

#include <boost/serialization/access.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/unordered_map.hpp>

class WordIdManager
{
public:
    uint32_t getId(const std::string& word);
    bool tryGet(const std::string& word, uint32_t& out) const noexcept;
    const std::string& byId(uint32_t id) const noexcept;
    size_t size() const noexcept;
    std::vector<uint32_t> bulkGetIds(const std::vector<std::string>& words);

    void rebuild(std::unordered_map<std::string, uint32_t>&& new_word2id,
                 std::vector<std::string>&& new_id2word);

private:
    //
    // Persistent layer (сериализуется)
    //
    std::unordered_map<std::string, uint32_t> word2id_;
    std::vector<std::string> id2word_;

    //
    // Быстрый слой — теперь через snapshot (атомарный shared_ptr)
    //
    using FastMap = robin_hood::unordered_map<std::string, uint32_t>;
    std::atomic<std::shared_ptr<FastMap>> fastSnapshot_;



    //
    // Mutex только для rare event — добавления нового слова
    //
    mutable std::mutex insertMutex_;

    std::atomic<uint32_t> nextId_{0};

    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive& ar, const unsigned int)
    {
        ar & word2id_;
        ar & id2word_;

        if constexpr (Archive::is_loading::value)
        {
            nextId_.store(id2word_.size(), std::memory_order_relaxed);

            auto newMap = std::make_shared<FastMap>();
            newMap->reserve(word2id_.size());
            for (auto& p : word2id_)
                (*newMap)[p.first] = p.second;

            fastSnapshot_.store(newMap, std::memory_order_release);
        }
    }
};
