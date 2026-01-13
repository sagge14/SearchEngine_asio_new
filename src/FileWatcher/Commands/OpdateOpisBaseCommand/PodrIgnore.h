#pragma once
#include <string>
#include <unordered_set>

/**
 * Хранит список подразделений, которые нужно игнорировать.
 * Данные читаются из текстового файла (по одной записи на строку).
 *
 * Формат файла:
 * ──────────────────────────
 * ИСХ
 * ТРАНЗИТ
 * ──────────────────────────
 *
 * Если файла нет – будет создан с двумя записями-по-умолчанию.
 */
class PodrIgnore {
public:
    /** @param file_path путь к ignore-файлу (относительный или абсолютный) */
    explicit PodrIgnore(std::string file_path = "ignore.txt");

    /** Проверить, входит ли `podr` в «чёрный» список. */
    [[nodiscard]] bool itsIgnore(const std::string& podr) const noexcept;

    /** Перечитать файл, если он изменился. */
    void reload();
    int size();
private:
    std::string                     file_path_;
    std::unordered_set<std::string> ignore_;

    void load();            // заполнить `ignore_` из файла
    void save_defaults();   // создать файл со значениями по умолчанию
};
