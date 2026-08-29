#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace inverted_index {

class InvertedIndex;

class IIndexSerializer {
public:
    virtual ~IIndexSerializer() = default;

    [[nodiscard]] virtual std::string kind() const = 0;
    [[nodiscard]] virtual bool exists() const = 0;

    virtual void save(const InvertedIndex& idx) = 0;
    virtual void load(InvertedIndex& idx) = 0;

    /* --- Инкрементальное (live) зеркало --------------------------------
       Поддерживается только хранилищами, способными править данные на лету
       (SQLite). Для остальных (Boost) методы — no-op, а supportsLiveUpdates()
       возвращает false. Live-вызовы потокобезопасны (enqueue в очередь). */

    [[nodiscard]] virtual bool supportsLiveUpdates() const { return false; }

    /// Открыть постоянное соединение/ресурсы для live-правок.
    virtual void openLive() {}

    /// Записать (или обновить) один файл одной транзакцией.
    /// widCounts — постинги файла (word_id -> count).
    /// newWords  — пары (word_id, word) для регистрации новых слов.
    /// wasUpdate — true, если содержимое файла менялось (старые постинги
    ///             файла нужно заменить).
    virtual void writeFile(uint32_t /*fileId*/,
                           const std::wstring& /*path*/,
                           int64_t /*mtimeTicks*/,
                           uint64_t /*size*/,
                           const std::vector<std::pair<uint32_t, uint16_t>>& /*widCounts*/,
                           const std::vector<std::pair<uint32_t, std::string>>& /*newWords*/,
                           bool /*wasUpdate*/) {}

    /// Пометить файл удалённым (deleted=1). Постинги сохраняются (вечный след).
    virtual void markFileDeleted(uint32_t /*fileId*/) {}

    /// Дождаться записи всех операций из очереди live-зеркала.
    virtual void flushPending() {}

    /// Сбросить WAL на диск (контрольная точка).
    virtual void checkpoint() {}
};

} // namespace inverted_index

