#include <vector>
#include <algorithm>
#include <cstdint>
#include <boost/serialization/vector.hpp>

struct Posting {
    uint32_t fileId;
    uint16_t cnt;

    // Для сериализации
    template<class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & fileId;
        ar & cnt;
    }
};

class PostingList {
    std::vector<Posting> data;

public:
    // Доступ (чтение): поиск по fileId — возвращает ptr на cnt, либо nullptr
    const uint16_t* find(uint32_t fileId) const {
        auto it = std::lower_bound(data.begin(), data.end(), fileId,
                                   [](const Posting& p, uint32_t id) { return p.fileId < id; });
        return (it != data.end() && it->fileId == fileId) ? &(it->cnt) : nullptr;
    }

    // Доступ (запись): оператор [], добавляет или увеличивает
    uint16_t& operator[](uint32_t fileId) {
        auto it = std::lower_bound(data.begin(), data.end(), fileId,
                                   [](const Posting& p, uint32_t id) { return p.fileId < id; });
        if (it != data.end() && it->fileId == fileId) {
            return it->cnt;
        }
        // Вставка нового элемента в нужное место
        it = data.insert(it, { fileId, 0 });
        return it->cnt;
    }

    // Удаление по fileId
    void erase(uint32_t fileId) {
        auto it = std::lower_bound(data.begin(), data.end(), fileId,
                                   [](const Posting& p, uint32_t id) { return p.fileId < id; });
        if (it != data.end() && it->fileId == fileId)
            data.erase(it);
    }

    // Итерация — для for(auto& p : postingList)
    auto begin() const { return data.begin(); }
    auto end()   const { return data.end(); }
    size_t size() const { return data.size(); }
    bool empty()  const { return data.empty(); }
    void clear()        { data.clear(); }

    // Для сериализации Boost
    template<class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & data;
    }
};