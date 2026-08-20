#include "MyUtils/FileScanner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace {

class TemporaryScanTree final
{
public:
    TemporaryScanTree()
    {
        static std::atomic_uint64_t sequence{0};
        const auto uniqueValue =
            std::chrono::steady_clock::now().time_since_epoch().count() +
            static_cast<std::int64_t>(
                sequence.fetch_add(1, std::memory_order_relaxed));
        root_ = fs::temp_directory_path() /
            ("se-filescanner-exclusion-" + std::to_string(uniqueValue));
        fs::create_directories(root_ / "keep");
        fs::create_directories(root_ / "excluded" / "nested");
        fs::create_directories(root_ / "excluded_similar");

        writeFile(root_ / "keep" / "a.txt", "012345678901234567890");
        writeFile(root_ / "keep" / "FILE.TXT", "012345678901234567890");
        writeFile(root_ / "keep" / "suffix.mytxt", "012345678901234567890");
        writeFile(root_ / "keep" / "README", "012345678901234567890");
        writeFile(root_ / "excluded" / "b.txt", "012345678901234567890");
        writeFile(root_ / "excluded" / "nested" / "c.txt", "012345678901234567890");
        writeFile(root_ / "excluded_similar" / "d.txt", "012345678901234567890");
    }

    ~TemporaryScanTree()
    {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] fs::path root() const { return root_; }

private:
    static void writeFile(const fs::path& path, const std::string& content)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    fs::path root_;
};

std::set<std::wstring> basenames(const std::vector<std::wstring>& paths)
{
    std::set<std::wstring> names;
    for (const auto& path : paths) {
        names.insert(fs::path(path).filename().wstring());
    }
    return names;
}

}  // namespace

TEST(FileScannerExclusionTest, FullScanSkipsExcludedSubtreeAndSimilarNames)
{
    TemporaryScanTree tree;
    const std::string rootUtf8 = tree.root().string();
    const std::string excludedUtf8 =
        (tree.root() / "excluded").string();

    const auto scanned = FileScanner::scanDirectories(
        {rootUtf8},
        file_extension_contract::Selection{{"txt"}, false},
        {excludedUtf8});

    const auto names = basenames(scanned);
    EXPECT_TRUE(names.count(L"a.txt") == 1u);
    EXPECT_TRUE(names.count(L"FILE.TXT") == 1u);
    EXPECT_TRUE(names.count(L"d.txt") == 1u);
    EXPECT_EQ(names.count(L"suffix.mytxt"), 0u);
    EXPECT_EQ(names.count(L"README"), 0u);
    EXPECT_EQ(names.count(L"b.txt"), 0u);
    EXPECT_EQ(names.count(L"c.txt"), 0u);
}
