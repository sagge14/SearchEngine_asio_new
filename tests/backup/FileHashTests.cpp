#include "Backup/FileHash.h"

#include <gtest/gtest.h>

TEST(FileHash, MatchesKnownSha256Vectors)
{
    EXPECT_EQ(
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855",
        sha256String("")
    );
    EXPECT_EQ(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad",
        sha256String("abc")
    );
}
