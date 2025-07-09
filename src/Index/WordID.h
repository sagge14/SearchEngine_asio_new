#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <boost/serialization/access.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/unordered_map.hpp>

/*  ──────────────────────────────────────────────────────────────
    Класс-утилита: двусторонний словарь
      • слово  → компактный uint32_t-ID
      • ID     → исходная строка
    Используется вместо хранения итераторов.
   ────────────────────────────────────────────────────────────── */
class WordIdManager
{
public:
    /** Вернуть ID, добавив слово при первом обращении */
    uint32_t getId(const std::string& word);

    /** Попытаться получить ID без создания новой записи
        (false, если слова ещё нет в словаре)               */
    bool     tryGet(const std::string& word, uint32_t& out) const noexcept;

    /** Обратное преобразование: ID → строка                */
    [[nodiscard]] const std::string& byId(uint32_t id) const noexcept;

    /** Сколько уникальных слов уже зарегистрировано        */
    [[nodiscard]] size_t   size() const noexcept;

    friend class boost::serialization::access;
    template<class Archive>
    void serialize(Archive& ar, const unsigned int /*version*/)
    {
        ar & word2id_;      // хранить не обязательно, но удобно
        ar & id2word_;
    }

private:
    std::unordered_map<std::string, uint32_t> word2id_;   // прямой словарь
    std::vector<std::string>                  id2word_;   // обратный
};
