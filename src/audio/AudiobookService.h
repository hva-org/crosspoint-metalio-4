#pragma once

// Background audiobook playback (Metalio only). One book is open at a time;
// it keeps playing while the user reads or browses.
//
// Ownership: the main loop calls every public method. Two FreeRTOS tasks live
// from open() to close(): the reader task copies compressed packets from the
// SD card into a PSRAM ring, and the decoder task decodes them, applies the
// speed, and writes 16 kHz PCM to HalAudio. The ring absorbs SD stalls while
// the reader holds the card.

#if CROSSPOINT_AUDIOBOOK

#include <AudioDecoder.h>
#include <BookModel.h>
#include <HalMemory.h>
#include <PcmPipeline.h>
#include <freertos/FreeRTOS.h>
#include <freertos/message_buffer.h>
#include <freertos/queue.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace audiobook {

struct ChapterTitle {
  char text[CHAPTER_TITLE_CAPACITY] = {};
};

enum class PlaybackProblem : uint8_t { None, Track, Output };

// What the player screen shows; copied out on the main loop.
struct PlayerSnapshot {
  bool open = false;
  bool playing = false;
  bool waitingForOutput = false;  // play pressed, headphones not connected yet
  bool finished = false;
  PlaybackProblem problem = PlaybackProblem::None;
  size_t chapter = 0;
  size_t chapterCount = 0;
  uint64_t chapterElapsedMs = 0;
  uint64_t chapterLengthMs = 0;  // 0 while unknown
  uint8_t speedIndex = DEFAULT_SPEED_INDEX;
  uint8_t volume = 0;
};

class AudiobookService {
 public:
  static AudiobookService& instance();

  // Opens the book `path` belongs to and resumes its saved position, paused.
  // An .mp3 inside a folder opens that folder's MP3s as one book; an
  // .m4b/.m4a, or an .mp3 at the card root, is a book of its own.
  bool open(const std::string& path);
  // Saves the position, stops the tasks and frees every buffer. Also called
  // before sleep, restarts, USB Drive, OTA, and changes to the book's files.
  void close();
  bool isOpen() const { return open_; }
  // Playing or waiting for the headphones; either keeps the device awake.
  bool isPlaying() const { return open_ && (playing_.load() || waitingForOutput_); }
  // True when `path` is the file the book was opened from or one of its tracks.
  bool owns(const std::string& path) const;
  // True when deleting or renaming `path` (a file or folder) changes this book.
  bool isAffectedBy(const std::string& path) const;

  const std::string& title() const { return title_; }
  const std::vector<ChapterTitle>& chapterTitles() const { return titles_; }

  void play();
  void pause();
  void toggle();
  void seekBy(int32_t seconds);
  void jumpToChapter(size_t index);
  void nextChapter();
  void previousChapter();
  void setSpeed(uint8_t index);
  void setVolume(uint8_t volume);

  PlayerSnapshot snapshot() const;
  // Changes with play state, chapter, track and problems (not every second).
  uint32_t revision() const { return revision_; }

  // Main-loop housekeeping: headphone link, track changes, end of book,
  // playback problems, periodic saves.
  void tick();

 private:
  AudiobookService() = default;

  // TrackError follows the packets read before a read error, so they still play.
  enum class MessageType : uint8_t { TrackStart, Packet, TrackEnd, TrackError };
  struct MessageHeader {
    MessageType type;
    Codec codec;
    uint16_t track;
    uint32_t generation;
    uint64_t startMs;
    AacConfig aac;
  };
  struct ReaderCommand {
    uint16_t track;
    uint32_t generation;
    uint64_t positionMs;
  };

  bool loadBook(const std::string& path);
  bool loadTrackNames(const std::string& folder);
  void loadChapters();
  size_t trackCount() const { return trackOffsets_.size(); }
  std::string trackPath(size_t index) const;
  bool allocate();
  void releaseBuffers();
  void forgetBook();
  void loadTrack(uint16_t track, uint64_t positionMs);
  bool startPlayback();
  void waitForOutput();
  void stopPlayback();
  void saveSettingsIfChanged();
  void savePosition();
  bool readSavedPosition(SavedPosition& out) const;
  size_t currentChapter() const;
  bool stopTasks();
  PlaybackProblem currentProblem() const;

  static void readerEntry(void* self);
  static void decoderEntry(void* self);
  void readerLoop();
  void decoderLoop();
  bool sendMessage(size_t len);
  bool drainPipeline(PcmPipeline& pipeline, uint32_t generation);
  void reportProblem(PlaybackProblem problem, uint32_t generation);

  // --- main loop state ---
  bool open_ = false;
  bool tasksStuck_ = false;
  std::string openedPath_;
  std::string bookRoot_;  // progressKey(): the folder or the single file
  std::string cacheDir_;
  std::string title_;
  // Tracks: a folder plus NUL-terminated names in one PSRAM block, so a long
  // folder does not sit in internal RAM as hundreds of small strings. A
  // single-file book has no folder and one name: the full path. The reader
  // task reads these through trackPath(), so they must not change between
  // open() and the tasks' exit.
  std::string trackFolder_;
  HalMemory::PsramBuffer trackNames_;
  std::vector<uint32_t> trackOffsets_;
  // Parallel: marks_[i] is where titles_[i] starts.
  std::vector<ChapterMark> marks_;
  std::vector<ChapterTitle> titles_;
  uint16_t track_ = 0;
  bool finished_ = false;
  bool waitingForOutput_ = false;
  unsigned long waitStartedMs_ = 0;
  bool audioAcquired_ = false;
  bool settingsUnsaved_ = false;
  size_t shownChapter_ = 0;
  PlaybackProblem shownProblem_ = PlaybackProblem::None;
  uint32_t revision_ = 0;
  unsigned long lastSaveMs_ = 0;

  // --- shared with the tasks ---
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> endedGeneration_{UINT32_MAX};
  std::atomic<uint32_t> problemGeneration_{UINT32_MAX};
  std::atomic<uint8_t> problem_{static_cast<uint8_t>(PlaybackProblem::None)};
  std::atomic<bool> playing_{false};
  std::atomic<bool> quit_{false};
  std::atomic<bool> decoderBusy_{false};
  std::atomic<bool> readerAlive_{false};
  std::atomic<bool> decoderAlive_{false};
  std::atomic<uint8_t> speedIndex_{DEFAULT_SPEED_INDEX};
  std::atomic<uint8_t> volume_{0};
  // Milliseconds within the current track; 32 bits keep the atomics lock-free.
  std::atomic<uint32_t> positionMs_{0};
  std::atomic<uint32_t> trackDurationMs_{0};

  HalMemory::PsramBuffer ringStorage_;
  StaticMessageBuffer_t ringControl_{};
  MessageBufferHandle_t ring_ = nullptr;
  QueueHandle_t commands_ = nullptr;
  TaskHandle_t decoderTask_ = nullptr;
  // Reader-task buffer.
  std::unique_ptr<uint8_t[]> readerPacket_;
  // Decoder-task buffers.
  std::unique_ptr<uint8_t[]> decoderPacket_;
  std::unique_ptr<int16_t[]> pcm_;
  std::unique_ptr<int16_t[]> mono_;
};

}  // namespace audiobook

#define AUDIOBOOK audiobook::AudiobookService::instance()

#endif
