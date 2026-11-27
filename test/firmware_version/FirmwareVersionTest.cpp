#include <gtest/gtest.h>

#include "FirmwareVersion.h"

using firmware_version::isNewer;

TEST(FirmwareVersion, SameVersionIsNotNewer) { EXPECT_FALSE(isNewer("2.0.0-echo.1", "2.0.0-echo.1")); }

TEST(FirmwareVersion, EchoCounterComparesNumerically) {
  EXPECT_TRUE(isNewer("2.0.0-echo.2", "2.0.0-echo.1"));
  EXPECT_TRUE(isNewer("2.0.0-echo.10", "2.0.0-echo.9"));
  EXPECT_FALSE(isNewer("2.0.0-echo.1", "2.0.0-echo.2"));
  EXPECT_FALSE(isNewer("2.0.0-echo.9", "2.0.0-echo.10"));
}

TEST(FirmwareVersion, UpstreamBaseOutranksEchoCounter) {
  EXPECT_TRUE(isNewer("2.1.0-echo.1", "2.0.0-echo.9"));
  EXPECT_TRUE(isNewer("3.0.0-echo.1", "2.9.9-echo.9"));
  EXPECT_TRUE(isNewer("2.0.1-echo.1", "2.0.0-echo.9"));
  EXPECT_FALSE(isNewer("2.0.0-echo.9", "2.1.0-echo.1"));
}

TEST(FirmwareVersion, UpstreamVersionSortsBeforeFirstEchoRelease) {
  EXPECT_TRUE(isNewer("2.0.0-echo.1", "2.0.0"));
  EXPECT_FALSE(isNewer("2.0.0", "2.0.0-echo.1"));
}

TEST(FirmwareVersion, DevBuildOnlyUpdatesToALaterRelease) {
  EXPECT_FALSE(isNewer("2.0.0-echo.1", "2.0.0-echo.1-dev-feat/echopoint-170719d"));
  EXPECT_TRUE(isNewer("2.0.0-echo.2", "2.0.0-echo.1-dev-feat/echopoint-170719d"));
}

TEST(FirmwareVersion, ReleaseCandidateUpdatesToItsRelease) {
  EXPECT_TRUE(isNewer("2.0.0-echo.1", "2.0.0-echo.1-rc+170719d"));
  EXPECT_FALSE(isNewer("2.0.0-echo.1", "2.0.0-echo.2-rc+170719d"));
  EXPECT_TRUE(isNewer("2.0.0", "2.0.0-rc+170719d"));
}

TEST(FirmwareVersion, PrefixedTagIsNotNewer) {
  // OTA builds the asset name from the bare tag; "v" tags read as 0.0.0.
  EXPECT_FALSE(isNewer("v2.0.0-echo.2", "2.0.0-echo.1"));
}
