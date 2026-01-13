#include "WordId.h"
#include <mutex>
#include <utility>   // std::pair, try_emplace
#include <cassert>   // debug-проверки (необязательно)

/* ─────────  прямое преобразование  ───────── */
uint32_t WordIdManager::getId(const std::string& w)
{

    auto [it, inserted] =
            word2id_.try_emplace(w, static_cast<uint32_t>(id2word_.size()));

    if (inserted)
        id2word_.push_back(w);           // индекс == только что выданному ID

    return it->second;
}

/* ─────────  проверка на существование  ───── */
bool WordIdManager::tryGet(const std::string& w, uint32_t& out) const noexcept
{

    auto it = word2id_.find(w);
    if (it == word2id_.end()) return false;
    out = it->second;
    return true;
}

/* ─────────  обратное преобразование  ─────── */
const std::string& WordIdManager::byId(uint32_t id) const noexcept
{

    assert(id < id2word_.size());
    return id2word_[id];
}

/* ─────────  сервис  ───────────────────────── */
size_t WordIdManager::size() const noexcept
{

    return id2word_.size();
}

void WordIdManager::rebuild(std::unordered_map<std::string, uint32_t>&& new_word2id,
                            std::vector<std::string>&& new_id2word) {

    word2id_ = std::move(new_word2id);
    id2word_ = std::move(new_id2word);
}
