#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace inverted_index::batch {

template<typename T>
class BoundedByteQueue final {
public:
    explicit BoundedByteQueue(std::size_t capacityBytes)
        : capacityBytes_(capacityBytes == 0 ? 1 : capacityBytes)
    {
    }

    BoundedByteQueue(const BoundedByteQueue&) = delete;
    BoundedByteQueue& operator=(const BoundedByteQueue&) = delete;

    bool push(T item, std::size_t weightBytes)
    {
        const std::size_t weight = weightBytes == 0 ? 1 : weightBytes;
        if (weight > capacityBytes_)
            throw std::length_error("item exceeds byte queue capacity");

        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this, weight] {
            return cancelled_ || closed_ ||
                   (queuedBytes_ <= capacityBytes_ &&
                    weight <= capacityBytes_ - queuedBytes_);
        });

        if (cancelled_ || closed_)
            return false;

        queue_.push_back(Entry{std::move(item), weight});
        queuedBytes_ += weight;
        notEmpty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] {
            return cancelled_ || closed_ || !queue_.empty();
        });

        if (cancelled_ || queue_.empty())
            return std::nullopt;

        Entry entry = std::move(queue_.front());
        queue_.pop_front();
        queuedBytes_ -= entry.weightBytes;
        notFull_.notify_all();
        return std::move(entry.value);
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void cancel() noexcept
    {
        std::deque<Entry> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            discarded.swap(queue_);
            queuedBytes_ = 0;
            notEmpty_.notify_all();
            notFull_.notify_all();
        }
    }

private:
    struct Entry {
        T value;
        std::size_t weightBytes{};
    };

    const std::size_t capacityBytes_;
    std::deque<Entry> queue_;
    std::size_t queuedBytes_{};
    bool closed_{};
    bool cancelled_{};
    std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
};

} // namespace inverted_index::batch
