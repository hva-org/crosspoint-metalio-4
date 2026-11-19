#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "BookModel.h"

using namespace audiobook;

TEST(BookModel, RecognisesAudiobookFiles) {
  EXPECT_TRUE(isAudiobookFile("Track 01.MP3"));
  EXPECT_TRUE(isAudiobookFile("book.m4b"));
  EXPECT_TRUE(isAudiobookFile("book.M4A"));
  EXPECT_FALSE(isAudiobookFile("book.epub"));
  EXPECT_FALSE(isAudiobookFile("mp3"));
  EXPECT_FALSE(isAudiobookFile("notes.mp3.txt"));
  EXPECT_TRUE(isMp4Book("x.m4b"));
  EXPECT_FALSE(isMp4Book("x.mp3"));
}

TEST(BookModel, ProgressKeyIsTheFolderOfAnMp3Book) {
  EXPECT_EQ(progressKey("/Books/Dune/Part 01.mp3"), "/Books/Dune");
  EXPECT_EQ(progressKey("/Books/Dune.m4b"), "/Books/Dune.m4b");
  EXPECT_EQ(progressKey("/loose.mp3"), "/loose.mp3");
  EXPECT_EQ(progressDir("/Books/Dune/Part 01.mp3"), progressDir("/Books/Dune/Part 02.mp3"));
  EXPECT_NE(progressDir("/a.m4b"), progressDir("/b.m4b"));
  EXPECT_EQ(progressDir("/a.m4b").rfind("/.crosspoint/audio_", 0), 0u);
  EXPECT_EQ(folderProgressDir("/Books/Dune/"), progressDir("/Books/Dune/Part 01.mp3"));
  EXPECT_EQ(folderProgressDir("/Books/Dune"), progressDir("/Books/Dune/Part 01.mp3"));
}

TEST(BookModel, AffectsPathCoversTheFileAndItsFolders) {
  EXPECT_TRUE(affectsPath("/Books/Dune", "/Books/Dune"));
  EXPECT_TRUE(affectsPath("/Books", "/Books/Dune/Part 01.mp3"));
  EXPECT_TRUE(affectsPath("/Books/", "/Books/Dune"));
  EXPECT_TRUE(affectsPath("/", "/Books/Dune"));
  EXPECT_FALSE(affectsPath("/Books/Dun", "/Books/Dune"));
  EXPECT_FALSE(affectsPath("/Books/Dune/Part 01.mp3", "/Books/Dune"));
  EXPECT_FALSE(affectsPath("/Other.epub", "/Books/Dune"));
}

TEST(BookModel, NaturalOrderComparesNumbersByValue) {
  std::vector<std::string> names = {"Part 10.mp3", "part 2.mp3", "Part 1.mp3", "Intro.mp3", "Part 02b.mp3"};
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) { return naturalCompare(a, b) < 0; });
  const std::vector<std::string> expected = {"Intro.mp3", "Part 1.mp3", "part 2.mp3", "Part 02b.mp3", "Part 10.mp3"};
  EXPECT_EQ(names, expected);
  EXPECT_EQ(naturalCompare("a01", "a1"), 0);
  EXPECT_LT(naturalCompare("a", "a1"), 0);
  EXPECT_GT(naturalCompare("track 100", "track 99"), 0);
}

TEST(BookModel, ChapterAtFindsTheContainingChapter) {
  const ChapterMark marks[] = {{0, 0}, {0, 60000}, {1, 0}, {1, 30000}};
  EXPECT_EQ(chapterAt(marks, 4, 0, 0), 0u);
  EXPECT_EQ(chapterAt(marks, 4, 0, 59999), 0u);
  EXPECT_EQ(chapterAt(marks, 4, 0, 60000), 1u);
  EXPECT_EQ(chapterAt(marks, 4, 1, 10), 2u);
  EXPECT_EQ(chapterAt(marks, 4, 1, 999999), 3u);
  EXPECT_EQ(chapterAt(marks, 0, 3, 5), 0u);
}

TEST(BookModel, ProgressRoundTripsAndRejectsBadData) {
  uint8_t bytes[PROGRESS_BYTES];
  encodeProgress({513, 3600123}, bytes);
  SavedPosition p;
  ASSERT_TRUE(decodeProgress(bytes, sizeof(bytes), p));
  EXPECT_EQ(p.track, 513);
  EXPECT_EQ(p.positionMs, 3600123u);

  EXPECT_FALSE(decodeProgress(bytes, sizeof(bytes) - 1, p));
  bytes[0] = 99;
  EXPECT_FALSE(decodeProgress(bytes, sizeof(bytes), p));
  EXPECT_FALSE(decodeProgress(nullptr, 7, p));
}
