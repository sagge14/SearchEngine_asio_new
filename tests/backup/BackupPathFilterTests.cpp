#include "Backup/BackupPathFilter.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

BackupPathFilter filterFrom(std::initializer_list<const char*> patterns)
{
    std::vector<std::string> values;
    for (const char* pattern : patterns) {
        std::string normalized;
        std::string error;
        EXPECT_TRUE(validateExcludePattern(pattern, normalized, error))
            << pattern << ": " << error;
        values.push_back(std::move(normalized));
    }
    return BackupPathFilter(std::move(values));
}

TEST(BackupPathFilter, EmptyFilterMatchesNothing)
{
    const BackupPathFilter filter;
    EXPECT_TRUE(filter.empty());
    EXPECT_FALSE(filter.isFileExcluded(fs::path("a.txt")));
    EXPECT_FALSE(filter.isDirectoryExcluded(fs::path("out")));
}

TEST(BackupPathFilter, DirectoryNameAtAnyDepth)
{
    const BackupPathFilter filter = filterFrom({"__astcache/"});
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("__astcache")));
    EXPECT_TRUE(
        filter.isDirectoryExcluded(fs::path("project") / "__astcache")
    );
    EXPECT_FALSE(filter.isFileExcluded(fs::path("__astcache")));
    EXPECT_FALSE(
        filter.isDirectoryExcluded(fs::path("__astcache_backup"))
    );
    EXPECT_FALSE(
        filter.isDirectoryExcluded(fs::path("project") / "__astcache_x")
    );
}

TEST(BackupPathFilter, RootAnchoredDirectory)
{
    const BackupPathFilter filter = filterFrom({"/__astcache/"});
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("__astcache")));
    EXPECT_FALSE(
        filter.isDirectoryExcluded(fs::path("nested") / "__astcache")
    );
}

TEST(BackupPathFilter, TrailingSlashMeansDirectoryOnly)
{
    const BackupPathFilter filter = filterFrom({".vs/"});
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path(".vs")));
    EXPECT_FALSE(filter.isFileExcluded(fs::path(".vs")));
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("src") / ".vs"));
}

TEST(BackupPathFilter, StarQuestionAndDoubleStar)
{
    const BackupPathFilter star = filterFrom({"*.obj"});
    EXPECT_TRUE(star.isFileExcluded(fs::path("a.obj")));
    EXPECT_TRUE(star.isFileExcluded(fs::path("build") / "a.obj"));
    EXPECT_FALSE(star.isFileExcluded(fs::path("a.obj.txt")));

    const BackupPathFilter question = filterFrom({"file?.txt"});
    EXPECT_TRUE(question.isFileExcluded(fs::path("file1.txt")));
    EXPECT_FALSE(question.isFileExcluded(fs::path("file10.txt")));

    const BackupPathFilter nested = filterFrom({"generated/**/*.tmp"});
    EXPECT_TRUE(nested.isFileExcluded(fs::path("generated") / "a.tmp"));
    EXPECT_TRUE(
        nested.isFileExcluded(fs::path("generated") / "x" / "y" / "a.tmp")
    );
    EXPECT_FALSE(nested.isFileExcluded(fs::path("other") / "a.tmp"));

    const BackupPathFilter cache = filterFrom({"**/cache/*.bin"});
    EXPECT_TRUE(cache.isFileExcluded(fs::path("cache") / "a.bin"));
    EXPECT_TRUE(cache.isFileExcluded(fs::path("lib") / "cache" / "a.bin"));
    EXPECT_FALSE(
        cache.isFileExcluded(fs::path("cache") / "nested" / "a.bin")
    );
}

TEST(BackupPathFilter, ExtensionMaskAtDifferentDepths)
{
    const BackupPathFilter filter = filterFrom({"*.pch"});
    EXPECT_TRUE(filter.isFileExcluded(fs::path("main.pch")));
    EXPECT_TRUE(
        filter.isFileExcluded(fs::path("Win32") / "Release" / "main.pch")
    );
}

TEST(BackupPathFilter, WindowsSeparatorsInRelativePath)
{
    const BackupPathFilter filter = filterFrom({"out/"});
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("out")));
    EXPECT_TRUE(
        filter.isDirectoryExcluded(fs::path("project\\out"))
    );
    EXPECT_TRUE(
        filter.isFileExcluded(fs::path("src\\file.tmp")) == false
    );

    const BackupPathFilter tmp = filterFrom({"*.tmp"});
    EXPECT_TRUE(tmp.isFileExcluded(fs::path("src\\file.tmp")));
}

TEST(BackupPathFilter, WindowsCaseInsensitivity)
{
    const BackupPathFilter filter = filterFrom({"Thumbs.db", "__AstCache/"});
    EXPECT_TRUE(filter.isFileExcluded(fs::path("thumbs.db")));
    EXPECT_TRUE(filter.isFileExcluded(fs::path("THUMBS.DB")));
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("__astcache")));
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::path("__ASTCACHE")));
}

TEST(BackupPathFilter, UnicodeNames)
{
    // Use UTF-8 literals so the test does not depend on the source charset.
    const std::string cache_dir = reinterpret_cast<const char*>(u8"кэш/");
    const std::string file_tmp = reinterpret_cast<const char*>(u8"файл.tmp");
    const std::string file_txt = reinterpret_cast<const char*>(u8"файл.txt");
    const std::string cache_name = reinterpret_cast<const char*>(u8"кэш");

    std::string normalized;
    std::string error;
    ASSERT_TRUE(validateExcludePattern("*.tmp", normalized, error));
    std::string cache_pattern;
    ASSERT_TRUE(validateExcludePattern(cache_dir, cache_pattern, error));
    const BackupPathFilter filter(
        std::vector<std::string>{normalized, cache_pattern}
    );

    EXPECT_TRUE(filter.isFileExcluded(fs::u8path(file_tmp)));
    EXPECT_TRUE(filter.isDirectoryExcluded(fs::u8path(cache_name)));
    EXPECT_TRUE(
        filter.isDirectoryExcluded(fs::path("build") / fs::u8path(cache_name))
    );
    EXPECT_FALSE(filter.isFileExcluded(fs::u8path(file_txt)));
}

TEST(BackupPathFilter, SimilarNamesDoNotMatch)
{
    const BackupPathFilter filter = filterFrom({"__astcache/", "*.obj"});
    EXPECT_FALSE(filter.isDirectoryExcluded(fs::path("__astcaches")));
    EXPECT_FALSE(filter.isFileExcluded(fs::path("a.object")));
    EXPECT_FALSE(filter.isFileExcluded(fs::path("obj")));
}

TEST(BackupPathFilter, RejectsAbsoluteDotDotAndNegation)
{
    std::string normalized;
    std::string error;

    EXPECT_FALSE(validateExcludePattern("", normalized, error));
    EXPECT_FALSE(validateExcludePattern("   ", normalized, error));
    EXPECT_FALSE(validateExcludePattern("!foo", normalized, error));
    EXPECT_NE(std::string::npos, error.find("negative"));
    EXPECT_FALSE(validateExcludePattern("C:/temp", normalized, error));
    EXPECT_FALSE(validateExcludePattern("D:\\temp\\a", normalized, error));
    EXPECT_FALSE(validateExcludePattern("\\\\server\\share", normalized, error));
    EXPECT_FALSE(validateExcludePattern("//server/share", normalized, error));
    EXPECT_FALSE(validateExcludePattern("../secret", normalized, error));
    EXPECT_FALSE(validateExcludePattern("foo/../bar", normalized, error));
    EXPECT_FALSE(validateExcludePattern(".", normalized, error));
}

TEST(BackupPathFilter, NormalizesValidatedPatterns)
{
    std::string normalized;
    std::string error;
    ASSERT_TRUE(validateExcludePattern("__astcache\\", normalized, error));
    EXPECT_EQ("__astcache/", normalized);
    ASSERT_TRUE(validateExcludePattern("/Out/", normalized, error));
    EXPECT_EQ("/Out/", normalized);
}

} // namespace
