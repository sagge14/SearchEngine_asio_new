#include "MyUtils/WindowsPath.h"

#include <gtest/gtest.h>

TEST(WindowsPathTest, LocalDrivePaths)
{
    EXPECT_TRUE(windows_path::isAbsoluteWindowsLocalPath("D:\\DATA"));
    EXPECT_TRUE(windows_path::isAbsoluteWindowsLocalPath("D:/DATA"));
    EXPECT_TRUE(windows_path::isAbsoluteWindowsFilesystemPath("D:\\DATA"));
    EXPECT_TRUE(windows_path::isAbsoluteWindowsFilesystemPath("D:/DATA"));
}

TEST(WindowsPathTest, UncPaths)
{
    EXPECT_FALSE(windows_path::isAbsoluteWindowsLocalPath("\\\\server\\share\\DATA"));
    EXPECT_TRUE(windows_path::isAbsoluteWindowsFilesystemPath("\\\\server\\share\\DATA"));
    EXPECT_TRUE(windows_path::isAbsoluteWindowsFilesystemPath("//server/share/DATA"));
}

TEST(WindowsPathTest, RejectsRelativeAndIncomplete)
{
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath("DATA"));
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath(".\\DATA"));
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath("..\\DATA"));
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath("D:DATA"));
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath(""));
    EXPECT_FALSE(windows_path::isAbsoluteWindowsFilesystemPath("relative\\data"));
}
