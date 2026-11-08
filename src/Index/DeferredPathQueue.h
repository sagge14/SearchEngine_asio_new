#pragma once

#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace inverted_index {

class DeferredPathQueue {
public:
    bool defer(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return paths_.insert(path).second;
    }

    [[nodiscard]] std::vector<std::wstring> takeAll()
    {
        std::unordered_set<std::wstring> paths;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paths.swap(paths_);
        }
        return {std::make_move_iterator(paths.begin()),
                std::make_move_iterator(paths.end())};
    }

    template<typename Replay>
    std::size_t drainOnce(Replay&& replay)
    {
        std::vector<std::wstring> paths = takeAll();
        for (const std::wstring& path : paths)
            replay(path);
        return paths.size();
    }

    void clear() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paths_.clear();
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return paths_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::wstring> paths_;
};

} // namespace inverted_index
