#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BtModuleProtocol.h"

using namespace metalio_bt;

namespace {
std::vector<std::string> feedAll(LineReader& reader, const std::string& bytes) {
  std::vector<std::string> lines;
  for (const char c : bytes) {
    if (reader.feed(c)) lines.emplace_back(reader.line());
  }
  return lines;
}
}  // namespace

TEST(BtModuleLineReader, SplitsCrLfLinesAndSkipsEmptyOnes) {
  LineReader reader;
  const auto lines = feedAll(reader, "SET MODE 2\r\n\r\nINQUIRING START\r\nAT+BT:AABBCCDDEEFFPhones\n");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "SET MODE 2");
  EXPECT_EQ(lines[1], "INQUIRING START");
  EXPECT_EQ(lines[2], "AT+BT:AABBCCDDEEFFPhones");
}

TEST(BtModuleLineReader, AcceptsBareCrLineEnds) {
  metalio_bt::LineReader reader;
  const auto lines = feedAll(reader, "SET MODE 2\rINQUIRING START\r");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "SET MODE 2");
  EXPECT_EQ(lines[1], "INQUIRING START");
}

TEST(BtModuleLineReader, FlushHandsOverAnUnterminatedLineOnce) {
  metalio_bt::LineReader reader;
  for (const char c : std::string("SET MODE 2")) EXPECT_FALSE(reader.feed(c));
  EXPECT_TRUE(reader.hasPartial());
  ASSERT_TRUE(reader.flush());
  EXPECT_STREQ(reader.line(), "SET MODE 2");
  EXPECT_FALSE(reader.hasPartial());
  EXPECT_FALSE(reader.flush());
  // A line end right after the flush does not repeat the line.
  EXPECT_FALSE(reader.feed('\r'));
  EXPECT_FALSE(reader.feed('\n'));
  const auto lines = feedAll(reader, "INQ COMPLETE\r\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "INQ COMPLETE");
}

TEST(BtModuleLineReader, DropsOverlongLineWholeAndRecovers) {
  LineReader reader;
  const std::string longLine(200, 'x');
  const auto lines = feedAll(reader, longLine + "\r\nCONNECT SUCCESS\r\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "CONNECT SUCCESS");
}

TEST(BtModuleParse, DeviceFoundNormalizesAddressAndKeepsName) {
  const Event e = parseLine("AT+BT:a1b2c3d4e5f6My Headphones  ");
  EXPECT_EQ(e.type, EventType::DeviceFound);
  EXPECT_STREQ(e.address, "A1B2C3D4E5F6");
  EXPECT_STREQ(e.name, "My Headphones");
}

TEST(BtModuleParse, DeviceFoundWithoutName) {
  const Event e = parseLine("AT+BT:001122334455");
  EXPECT_EQ(e.type, EventType::DeviceFound);
  EXPECT_STREQ(e.address, "001122334455");
  EXPECT_STREQ(e.name, "");
}

TEST(BtModuleParse, DeviceFoundRejectsShortOrNonHexAddress) {
  EXPECT_EQ(parseLine("AT+BT:0011223344").type, EventType::None);
  EXPECT_EQ(parseLine("AT+BT:00112233445GName").type, EventType::None);
}

TEST(BtModuleParse, LongUtf8NameIsCutOnCharacterBoundary) {
  // 30 ASCII bytes then a 3-byte character that straddles the 31-byte limit.
  const std::string name = std::string(30, 'a') + "\xE2\x82\xAC" + "tail";
  const Event e = parseLine(("AT+BT:AABBCCDDEEFF" + name).c_str());
  ASSERT_EQ(e.type, EventType::DeviceFound);
  EXPECT_EQ(std::string(e.name), std::string(30, 'a'));
}

TEST(BtModuleParse, StatusLines) {
  EXPECT_EQ(parseLine("SET MODE 1").type, EventType::ModeSet);
  EXPECT_EQ(parseLine("SET MODE 2").mode, 2);
  EXPECT_EQ(parseLine("SET MODE 9").type, EventType::None);
  EXPECT_EQ(parseLine("INQUIRING START").type, EventType::ScanStarted);
  EXPECT_EQ(parseLine("INQ COMPLETE").type, EventType::ScanDone);
  EXPECT_EQ(parseLine("CONNECTING").type, EventType::Connecting);
  EXPECT_EQ(parseLine("CONNECT SUCCESS").type, EventType::Connected);
  EXPECT_EQ(parseLine("CONNECT TIMEOUT").type, EventType::ConnectTimeout);
  EXPECT_EQ(parseLine("REMOTE DISCON").type, EventType::Disconnected);
  // Lines seen on the device that must not start anything.
  EXPECT_EQ(parseLine("SLC CONNECT SUCCESS").type, EventType::None);
  EXPECT_EQ(parseLine("DISC SCO").type, EventType::None);
  EXPECT_EQ(parseLine("AT+RX=1").type, EventType::None);
  EXPECT_EQ(parseLine("OK").type, EventType::None);
  EXPECT_EQ(parseLine(nullptr).type, EventType::None);
}

TEST(BtModuleParse, ReconnectWaitsForSuccess) {
  EXPECT_EQ(parseLine("RECONNECT").type, EventType::Reconnect);
  EXPECT_EQ(parseLine("RECONNECT SUCCESS").type, EventType::Connected);
}

TEST(BtModuleFormat, ConnectCommand) {
  char out[32];
  const size_t n = formatConnect("AABBCCDDEEFF", out, sizeof(out));
  EXPECT_EQ(n, std::strlen("AT+CONNECT=AABBCCDDEEFF\r\n"));
  EXPECT_STREQ(out, "AT+CONNECT=AABBCCDDEEFF\r\n");
}

TEST(BtModuleFormat, ConnectRejectsBadInput) {
  char out[32];
  EXPECT_EQ(formatConnect("AABBCCDDEEF", out, sizeof(out)), 0u);
  EXPECT_EQ(formatConnect("AABBCCDDEEFF0", out, sizeof(out)), 0u);
  EXPECT_EQ(formatConnect("AABBCCDDEEFF", out, 25), 0u);  // needs 26 with terminator
  EXPECT_EQ(formatConnect(nullptr, out, sizeof(out)), 0u);
}
