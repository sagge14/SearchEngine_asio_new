#include "WordID.h"
#include <cassert>

/*─────────────────────────────────────────────────────────────
 *  Popытаться получить ID без вставки (быстрый путь)
 *────────────────────────────────────────────────────────────*/
bool WordIdManager::tryGet(const std::string& w, uint32_t& out) const noexcept
{
    auto snap = fastSnapshot_.load(std::memory_order_acquire);
    if (snap)
    {
        auto it = snap->find(w);
        if (it != snap->end())
        {
            out = it->second;
            return true;
        }
    }

    return false;
}

/*─────────────────────────────────────────────────────────────
 *  Вернуть ID (добавив слово при первом обращении)
 *────────────────────────────────────────────────────────────*/
uint32_t WordIdManager::getId(const std::string& w)
{
    uint32_t id;

    // 1) быстрый lock-free путь
    if (tryGet(w, id))
        return id;

    // 2) редкий путь — вставка нового слова
    std::lock_guard<std::mutex> lock(insertMutex_);

    // double-check (пока мы шли, другое слово могло вставиться)
    auto it = word2id_.find(w);
    if (it != word2id_.end())
        return it->second;

    // 3) создаём новый ID
    id = nextId_.fetch_add(1, std::memory_order_relaxed);

    word2id_[w] = id;
    id2word_.push_back(w);

    // 4) пересоздаём fastSnapshot
    auto oldSnap = fastSnapshot_.load(std::memory_order_acquire);
    auto newSnap = std::make_shared<FastMap>(oldSnap ? *oldSnap : FastMap{});
    (*newSnap)[w] = id;

    // 5) публикуем snapshot (гарантия видимости)
    fastSnapshot_.store(newSnap, std::memory_order_release);

    return id;
}

/*─────────────────────────────────────────────────────────────*/
const std::string& WordIdManager::byId(uint32_t id) const noexcept
{
    assert(id < id2word_.size());
    return id2word_[id];
}

/*─────────────────────────────────────────────────────────────*/
size_t WordIdManager::size() const noexcept
{
    return id2word_.size();
}


std::vector<uint32_t> WordIdManager::bulkGetIds(const std::vector<std::string>& words)
{
    std::vector<uint32_t> ids(words.size());
    ids.reserve(words.size());

    // Список слов, которых нет в snapshot
    std::vector<std::string> newWords;
    newWords.reserve(words.size());

    auto snap = fastSnapshot_.load(std::memory_order_acquire);

    // === 1. Быстрый путь tryGet: уже существующие слова ===
    for (size_t i = 0; i < words.size(); ++i)
    {
        const std::string& w = words[i];

        if (snap)
        {
            auto it = snap->find(w);
            if (it != snap->end())
            {
                ids[i] = it->second;
                continue;
            }
        }

        // Новое слово — пока без ID
        ids[i] = UINT32_MAX;
        newWords.push_back(w);
    }

    // === 2. Если нет новых слов — всё готово ===
    if (newWords.empty())
        return ids;

    // === 3. Вставляем ВСЕ новые слова под ОДНИМ mutex ===
    std::lock_guard<std::mutex> lock(insertMutex_);

    // double-check: потому что пока мы шли к mutex,
    // другие потоки могли вставить что-то
    snap = fastSnapshot_.load(std::memory_order_acquire);

    auto newSnap = std::make_shared<FastMap>(snap ? *snap : FastMap{});

    for (auto& w : newWords)
    {
        auto it = word2id_.find(w);
        uint32_t id;

        if (it != word2id_.end())
        {
            id = it->second;
        }
        else
        {
            id = nextId_.fetch_add(1, std::memory_order_relaxed);
            word2id_[w] = id;
            id2word_.push_back(w);
        }

        (*newSnap)[w] = id;
    }

    // Публикуем snapshot ОДИН раз
    fastSnapshot_.store(newSnap, std::memory_order_release);

    // === 4. Второй проход: выставляем wid для новых слов ===
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (ids[i] != UINT32_MAX)
            continue;

        ids[i] = (*newSnap)[words[i]];
    }

    return ids;
}


/*─────────────────────────────────────────────────────────────
 *  Пересборка при загрузке индекса
 *────────────────────────────────────────────────────────────*/
void WordIdManager::rebuild(
        std::unordered_map<std::string, uint32_t>&& new_word2id,
        std::vector<std::string>&& new_id2word)
{
    word2id_  = std::move(new_word2id);
    id2word_  = std::move(new_id2word);

    nextId_.store(id2word_.size(), std::memory_order_relaxed);

    auto newSnap = std::make_shared<FastMap>();
    newSnap->reserve(word2id_.size());

    for (auto& p : word2id_)
        (*newSnap)[p.first] = p.second;

    fastSnapshot_.store(newSnap, std::memory_order_release);
}
