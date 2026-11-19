#include "AudiobookService.h"

#if CROSSPOINT_AUDIOBOOK

#include <HalAudio.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Mp3Reader.h>
#include <Mp4Reader.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/reader/ProgressFile.h"
#include "audio/AudioOutput.h"
#include "audio/HalFileSource.h"

namespace audiobook {
namespace {
// About 30 s of 64 kbps audio: rides out SD stalls while the reader or a
// chapter build holds the card.
constexpr size_t RING_BYTES = 256 * 1024;
constexpr size_t READ_CACHE_BYTES = 16 * 1024;
// Chapter titles sit far apart in the file; a small window avoids reading a
// full cache window per title.
constexpr size_t CHAPTER_CACHE_BYTES = 1024;
constexpr size_t MONO_CHUNK = 512;
constexpr size_t MAX_TRACKS = 999;
constexpr size_t NAME_CAPACITY = 256;
// Sized with headroom over Helix and SdFat call depth; the tasks log their
// high-water marks on exit so these can be tightened from measurements.
constexpr uint32_t READER_STACK = 6144;
constexpr uint32_t DECODER_STACK = 10240;
constexpr UBaseType_t READER_PRIORITY = 4;
constexpr UBaseType_t DECODER_PRIORITY = 6;
constexpr BaseType_t AUDIO_CORE = 0;
constexpr TickType_t WAIT_TICKS = pdMS_TO_TICKS(50);
constexpr TickType_t IDLE_WAIT_TICKS = pdMS_TO_TICKS(200);
// The decoder yields at least this often, so a decode slower than real time
// cannot starve core 0's idle task (task watchdog).
constexpr TickType_t YIELD_EVERY_TICKS = pdMS_TO_TICKS(500);
constexpr uint32_t PAUSE_WAIT_MS = 500;
constexpr uint32_t STOP_WAIT_MS = 5000;
// HalAudio::write gives up after 100 ms without a module clock.
constexpr uint32_t WRITE_TIMEOUT_MS = 100;
constexpr uint32_t OUTPUT_STALL_MS = 2000;
// Covers a reconnect inside HalAudio::wakeOutput().
constexpr unsigned long LINK_WAIT_MS = 45000;

// The module is set up for the output: speaker mode, or a linked headset told
// to play.
bool outputSetUp() {
  return halAudio.output() == HalAudio::Output::Headphones ? halAudio.headsetReady() : halAudio.modeSettled();
}

// Playback may start: set up, and a check found the module clocking the bus.
bool outputReady() { return outputSetUp() && halAudio.clockConfirmed(); }

// Playing must stop: the mode changed or the headset link went away. Looser
// than !outputReady(), so a queued module command does not stop playback.
bool outputLost() {
  return halAudio.output() == HalAudio::Output::Headphones ? !halAudio.headsetConnected() : !halAudio.modeSettled();
}
constexpr unsigned long SAVE_INTERVAL_MS = 30000;
constexpr uint64_t RESTART_CHAPTER_MS = 3000;

std::string lastComponent(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string fileStem(const std::string& path) {
  std::string name = lastComponent(path);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.resize(dot);
  return name;
}

void setTitle(ChapterTitle& title, const std::string& text) {
  copyTitle(text.c_str(), text.size(), title.text, sizeof(title.text));
}

bool isTrackFile(HalFile& file, const char* name) {
  return !file.isDirectory() && name[0] != '.' && isAudiobookFile(name) && !isMp4Book(name);
}
}  // namespace

AudiobookService& AudiobookService::instance() {
  static AudiobookService service;
  return service;
}

// ---------------------------------------------------------------------------
// The book: tracks and chapters

std::string AudiobookService::trackPath(const size_t index) const {
  const char* name = reinterpret_cast<const char*>(trackNames_.get()) + trackOffsets_[index];
  return trackFolder_.empty() ? std::string(name) : trackFolder_ + "/" + name;
}

bool AudiobookService::owns(const std::string& path) const {
  if (!open_) return false;
  if (path == openedPath_) return true;
  const char* names = reinterpret_cast<const char*>(trackNames_.get());
  if (trackFolder_.empty()) return trackCount() == 1 && path == names;
  // Compare the name in place: this runs for every file a folder delete removes.
  if (path.size() <= trackFolder_.size() + 1 || path.compare(0, trackFolder_.size(), trackFolder_) != 0 ||
      path[trackFolder_.size()] != '/') {
    return false;
  }
  const char* name = path.c_str() + trackFolder_.size() + 1;
  for (const uint32_t offset : trackOffsets_) {
    if (strcmp(names + offset, name) == 0) return true;
  }
  return false;
}

bool AudiobookService::isAffectedBy(const std::string& path) const {
  return open_ && (affectsPath(path, bookRoot_) || owns(path));
}

bool AudiobookService::loadTrackNames(const std::string& folder) {
  // Two passes: size one PSRAM block for the names, then fill it.
  auto dir = Storage.open(folder.c_str());
  if (!dir || !dir.isDirectory()) return false;
  char name[NAME_CAPACITY];
  size_t count = 0, bytes = 0;
  for (auto file = dir.openNextFile(); file && count < MAX_TRACKS; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (!isTrackFile(file, name)) continue;
    count++;
    bytes += strlen(name) + 1;
  }
  if (count == 0) return false;
  trackNames_ = HalMemory::allocatePsram(bytes);
  if (!trackNames_) {
    LOG_ERR("ABK", "OOM: track names (%u B PSRAM)", static_cast<unsigned>(bytes));
    return false;
  }
  trackOffsets_.clear();
  trackOffsets_.reserve(count);
  dir.rewindDirectory();
  size_t used = 0;
  for (auto file = dir.openNextFile(); file && trackOffsets_.size() < count; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (!isTrackFile(file, name)) continue;
    const size_t len = strlen(name) + 1;
    if (used + len > bytes) break;  // the folder changed between the passes
    memcpy(trackNames_.get() + used, name, len);
    trackOffsets_.push_back(static_cast<uint32_t>(used));
    used += len;
  }
  const char* base = reinterpret_cast<const char*>(trackNames_.get());
  std::sort(trackOffsets_.begin(), trackOffsets_.end(),
            [base](const uint32_t a, const uint32_t b) { return naturalCompare(base + a, base + b) < 0; });
  trackFolder_ = folder;
  return !trackOffsets_.empty();
}

bool AudiobookService::loadBook(const std::string& path) {
  openedPath_ = path;
  bookRoot_ = progressKey(path);
  cacheDir_ = progressDir(path);
  trackFolder_.clear();
  trackNames_.reset();
  trackOffsets_.clear();
  const bool folderBook = bookRoot_ != path;
  if (!folderBook || !loadTrackNames(bookRoot_)) {
    trackFolder_.clear();
    trackNames_ = HalMemory::allocatePsram(path.size() + 1);
    if (!trackNames_) return false;
    memcpy(trackNames_.get(), path.c_str(), path.size() + 1);
    trackOffsets_.assign(1, 0);
  }
  title_ = folderBook ? lastComponent(bookRoot_) : fileStem(path);
  loadChapters();
  return true;
}

void AudiobookService::loadChapters() {
  marks_.clear();
  titles_.clear();
  const std::string first = trackPath(0);
  if (isMp4Book(first)) {
    HalFileSource source;
    CachedReader cache;
    auto reader = makeUniqueNoThrow<Mp4Reader>();
    if (reader && source.open(first.c_str()) && cache.init(&source, CHAPTER_CACHE_BYTES) &&
        reader->open(source, cache, true) && reader->chapterCount() > 0) {
      const size_t count = reader->chapterCount();
      marks_.reserve(count);
      titles_.reserve(count);
      for (size_t i = 0; i < count; i++) {
        const Chapter& c = reader->chapters()[i];
        marks_.push_back({0, c.startMs});
        titles_.emplace_back();
        memcpy(titles_.back().text, c.title, sizeof(titles_.back().text));
      }
    }
  }
  if (marks_.empty()) {
    marks_.reserve(trackCount());
    titles_.reserve(trackCount());
    for (size_t i = 0; i < trackCount(); i++) {
      marks_.push_back({static_cast<uint16_t>(i), 0});
      titles_.emplace_back();
      setTitle(titles_.back(), fileStem(trackPath(i)));
    }
  }
}

void AudiobookService::forgetBook() {
  openedPath_.clear();
  bookRoot_.clear();
  cacheDir_.clear();
  title_.clear();
  trackFolder_.clear();
  trackNames_.reset();
  trackOffsets_.clear();
  marks_.clear();
  titles_.clear();
}

// ---------------------------------------------------------------------------
// Opening and closing

bool AudiobookService::allocate() {
  ringStorage_ = HalMemory::allocatePsram(RING_BYTES + 1);  // the buffer API needs one spare byte
  readerPacket_ = makeUniqueNoThrow<uint8_t[]>(sizeof(MessageHeader) + MAX_PACKET_BYTES);
  decoderPacket_ = makeUniqueNoThrow<uint8_t[]>(sizeof(MessageHeader) + MAX_PACKET_BYTES);
  pcm_ = makeUniqueNoThrow<int16_t[]>(MAX_DECODED_SAMPLES);
  mono_ = makeUniqueNoThrow<int16_t[]>(MONO_CHUNK);
  if (!ringStorage_ || !readerPacket_ || !decoderPacket_ || !pcm_ || !mono_) {
    LOG_ERR("ABK", "OOM: playback buffers (%u B PSRAM ring + %u B)", static_cast<unsigned>(RING_BYTES),
            static_cast<unsigned>(2 * (sizeof(MessageHeader) + MAX_PACKET_BYTES) +
                                  (MAX_DECODED_SAMPLES + MONO_CHUNK) * sizeof(int16_t)));
    return false;
  }
  ring_ = xMessageBufferCreateStatic(RING_BYTES, ringStorage_.get(), &ringControl_);
  // One slot: the newest seek or track change replaces one not yet taken.
  commands_ = xQueueCreate(1, sizeof(ReaderCommand));
  return ring_ != nullptr && commands_ != nullptr;
}

void AudiobookService::releaseBuffers() {
  if (commands_) vQueueDelete(commands_);
  if (ring_) vMessageBufferDelete(ring_);
  commands_ = nullptr;
  ring_ = nullptr;
  ringStorage_.reset();
  readerPacket_.reset();
  decoderPacket_.reset();
  pcm_.reset();
  mono_.reset();
}

bool AudiobookService::open(const std::string& path) {
  if (open_ && owns(path)) return true;
  close();
  if (tasksStuck_) {
    // Tasks that would not stop still use the old buffers, names and bus.
    if (readerAlive_ || decoderAlive_) {
      LOG_ERR("ABK", "Earlier playback tasks are still running");
      return false;
    }
    releaseBuffers();
    forgetBook();
    tasksStuck_ = false;
  }

  if (!loadBook(path) || !allocate()) {
    releaseBuffers();
    forgetBook();
    return false;
  }
  if (!ensureOutput()) {
    releaseBuffers();
    forgetBook();
    return false;
  }
  // The speaker played when its commands went out again for each book.
  halAudio.renewSpeakerMode();

  speedIndex_ = SETTINGS.audio.speedIndex;
  volume_ = SETTINGS.audio.volume;
  quit_ = false;
  playing_ = false;
  waitingForOutput_ = false;
  finished_ = false;
  generation_ = 0;
  endedGeneration_ = UINT32_MAX;
  problemGeneration_ = UINT32_MAX;
  shownProblem_ = PlaybackProblem::None;
  readerAlive_ = true;
  decoderAlive_ = true;
  if (xTaskCreatePinnedToCore(readerEntry, "abook_rd", READER_STACK, this, READER_PRIORITY, nullptr, AUDIO_CORE) !=
      pdPASS) {
    readerAlive_ = false;
    decoderAlive_ = false;
  } else if (xTaskCreatePinnedToCore(decoderEntry, "abook_dec", DECODER_STACK, this, DECODER_PRIORITY, &decoderTask_,
                                     AUDIO_CORE) != pdPASS) {
    decoderAlive_ = false;
  }
  if (!readerAlive_ || !decoderAlive_) {
    LOG_ERR("ABK", "Could not start playback tasks");
    if (!stopTasks()) {
      tasksStuck_ = true;
      return false;
    }
    releaseBuffers();
    forgetBook();
    return false;
  }

  // Resume where this book stopped; otherwise start at the opened track.
  uint16_t track = 0;
  uint64_t position = 0;
  SavedPosition saved;
  if (readSavedPosition(saved) && saved.track < trackCount()) {
    track = saved.track;
    position = saved.positionMs;
  } else {
    for (size_t i = 0; i < trackCount(); i++) {
      if (trackPath(i) == path) track = static_cast<uint16_t>(i);
    }
  }

  open_ = true;
  loadTrack(track, position);
  shownChapter_ = currentChapter();
  shownHeadphones_ = halAudio.output() == HalAudio::Output::Headphones;
  lastSaveMs_ = millis();
  // One Recents entry per book: a folder book is listed by its first track.
  RECENT_BOOKS.addBook(trackFolder_.empty() ? openedPath_ : trackPath(0), title_, "", "");
  RECENT_BOOKS.saveToFile();
  LOG_INF("ABK", "Opened %s: %u track(s), %u chapter(s)", title_.c_str(), static_cast<unsigned>(trackCount()),
          static_cast<unsigned>(marks_.size()));
  revision_++;
  return true;
}

bool AudiobookService::stopTasks() {
  // Both tasks wait at most 200 ms between checks of quit_.
  quit_ = true;
  const unsigned long start = millis();
  while ((readerAlive_ || decoderAlive_) && millis() - start < STOP_WAIT_MS) vTaskDelay(pdMS_TO_TICKS(10));
  decoderTask_ = nullptr;
  if (readerAlive_ || decoderAlive_) {
    LOG_ERR("ABK", "Playback task did not stop; keeping its buffers");
    return false;
  }
  return true;
}

void AudiobookService::close() {
  if (!open_) return;
  halAudio.cancelWake();  // also one still running after a timed-out wait
  waitingForOutput_ = false;
  stopPlayback();
  savePosition();
  saveSettingsIfChanged();
  open_ = false;
  revision_++;
  if (!stopTasks()) {
    // A task stuck in SD I/O still uses the buffers and the track names;
    // open() frees them once it has gone.
    tasksStuck_ = true;
    return;
  }
  releaseBuffers();
  forgetBook();
}

// ---------------------------------------------------------------------------
// Transport

void AudiobookService::reportProblem(const PlaybackProblem problem, const uint32_t generation) {
  problem_ = static_cast<uint8_t>(problem);
  problemGeneration_ = generation;
}

PlaybackProblem AudiobookService::currentProblem() const {
  return problemGeneration_.load() == generation_.load() ? static_cast<PlaybackProblem>(problem_.load())
                                                         : PlaybackProblem::None;
}

void AudiobookService::loadTrack(const uint16_t track, const uint64_t positionMs) {
  if (track != track_) trackDurationMs_ = 0;
  track_ = track;
  finished_ = false;
  positionMs_ = static_cast<uint32_t>(positionMs);
  // A new generation also clears any problem the previous one reported.
  const uint32_t generation = ++generation_;
  const ReaderCommand command{track, generation, positionMs};
  xQueueOverwrite(commands_, &command);
  revision_++;
}

bool AudiobookService::startPlayback() {
  if (!halAudio.startStream()) {
    LOG_ERR("ABK", "Audio output did not start");
    halAudio.note("playback: stream did not start");
    reportProblem(PlaybackProblem::Output, generation_);
    revision_++;
    return false;
  }
  playing_ = true;
  if (decoderTask_) xTaskNotifyGive(decoderTask_);
  revision_++;
  return true;
}

void AudiobookService::stopPlayback() {
  if (!playing_) return;
  playing_ = false;
  // The decoder finishes its current write first; the stream must not stop under it.
  const unsigned long start = millis();
  while (decoderBusy_ && millis() - start < PAUSE_WAIT_MS) vTaskDelay(pdMS_TO_TICKS(5));
  halAudio.stopStream();
}

void AudiobookService::play() {
  if (!open_ || playing_ || waitingForOutput_) return;
  halAudio.resetWake();  // a failed check may run again
  if (finished_) {
    loadTrack(0, 0);
  } else if (currentProblem() != PlaybackProblem::None) {
    loadTrack(track_, positionMs_.load());  // try the same place again
  }
  // Play once the output can be heard, so nothing is skipped.
  if (!outputReady()) {
    waitForOutput();
    return;
  }
  startPlayback();
}

void AudiobookService::waitForOutput() {
  // Retry the saved headphones after a timeout or a lost link.
  if (halAudio.output() == HalAudio::Output::Headphones && !halAudio.headsetConnected() && !halAudio.isBusy() &&
      halAudio.link() != HalAudio::Link::Connecting && SETTINGS.audio.headsetAddress[0] != '\0') {
    halAudio.connect(SETTINGS.audio.headsetAddress);
  }
  waitingForOutput_ = true;
  waitStartedMs_ = millis();
  waitHeadphones_ = halAudio.output() == HalAudio::Output::Headphones;
  revision_++;
}

void AudiobookService::pause() {
  if (!open_ || (!playing_ && !waitingForOutput_)) return;
  if (waitingForOutput_) halAudio.cancelWake();  // its checks would go on behind other screens
  waitingForOutput_ = false;
  stopPlayback();
  savePosition();
  saveSettingsIfChanged();
  revision_++;
}

void AudiobookService::toggle() {
  if (playing_ || waitingForOutput_) {
    pause();
  } else {
    play();
  }
}

void AudiobookService::seekBy(const int32_t seconds) {
  if (!open_) return;
  const int64_t target = static_cast<int64_t>(positionMs_.load()) + static_cast<int64_t>(seconds) * 1000;
  const uint64_t duration = trackDurationMs_.load();
  if (duration > 0 && target >= static_cast<int64_t>(duration)) {
    if (track_ + 1u < trackCount()) {
      loadTrack(track_ + 1, 0);
      return;
    }
    loadTrack(track_, duration > 1000 ? duration - 1000 : 0);
    return;
  }
  loadTrack(track_, target > 0 ? static_cast<uint64_t>(target) : 0);
}

void AudiobookService::jumpToChapter(const size_t index) {
  if (!open_ || index >= marks_.size()) return;
  loadTrack(marks_[index].track, marks_[index].startMs);
  savePosition();
}

void AudiobookService::nextChapter() {
  if (!open_ || marks_.empty()) return;
  const size_t chapter = currentChapter();
  if (chapter + 1 < marks_.size()) jumpToChapter(chapter + 1);
}

void AudiobookService::previousChapter() {
  if (!open_ || marks_.empty()) return;
  const size_t chapter = currentChapter();
  const uint64_t position = positionMs_.load();
  const bool sameTrack = marks_[chapter].track == track_;
  const uint64_t elapsed = sameTrack && position > marks_[chapter].startMs ? position - marks_[chapter].startMs : 0;
  // A few seconds in, "previous" restarts the chapter, as on most players.
  jumpToChapter(elapsed > RESTART_CHAPTER_MS || chapter == 0 ? chapter : chapter - 1);
}

void AudiobookService::setSpeed(const uint8_t index) {
  if (index >= PLAYBACK_SPEED_COUNT) return;
  speedIndex_ = index;
  SETTINGS.audio.speedIndex = index;
  settingsUnsaved_ = true;  // saved on pause or close, not on every press
  revision_++;
}

void AudiobookService::setVolume(const uint8_t volume) {
  const uint8_t v = volume > 100 ? 100 : volume;
  volume_ = v;
  SETTINGS.audio.volume = v;
  settingsUnsaved_ = true;
  revision_++;
}

void AudiobookService::saveSettingsIfChanged() {
  if (!settingsUnsaved_) return;
  SETTINGS.saveToFile();
  settingsUnsaved_ = false;
}

size_t AudiobookService::currentChapter() const {
  return chapterAt(marks_.data(), marks_.size(), track_, positionMs_.load());
}

PlayerSnapshot AudiobookService::snapshot() const {
  PlayerSnapshot s;
  s.open = open_;
  if (!open_ || marks_.empty()) return s;
  s.playing = playing_;
  s.waitingForOutput = waitingForOutput_;
  s.finished = finished_;
  s.problem = currentProblem();
  s.speedIndex = speedIndex_;
  s.volume = volume_;
  s.chapterCount = marks_.size();
  s.chapter = currentChapter();
  const ChapterMark& mark = marks_[s.chapter];
  const uint64_t position = positionMs_.load();
  s.chapterElapsedMs = mark.track == track_ && position > mark.startMs ? position - mark.startMs : 0;
  const bool nextInTrack = s.chapter + 1 < marks_.size() && marks_[s.chapter + 1].track == mark.track;
  const uint64_t end = nextInTrack ? marks_[s.chapter + 1].startMs : trackDurationMs_.load();
  s.chapterLengthMs = end > mark.startMs ? end - mark.startMs : 0;
  return s;
}

bool AudiobookService::readSavedPosition(SavedPosition& out) const {
  HalFile file;
  if (!Storage.openFileForRead("ABK", cacheDir_ + "/progress.bin", file)) return false;
  uint8_t bytes[PROGRESS_BYTES];
  return file.read(bytes, sizeof(bytes)) == static_cast<int>(sizeof(bytes)) &&
         decodeProgress(bytes, sizeof(bytes), out);
}

void AudiobookService::savePosition() {
  if (cacheDir_.empty() || trackCount() == 0) return;
  if (!Storage.ensureDirectoryExists(cacheDir_.c_str())) return;
  uint8_t bytes[PROGRESS_BYTES];
  const uint32_t position = finished_ ? 0 : positionMs_.load();
  encodeProgress({finished_ ? static_cast<uint16_t>(0) : track_, position}, bytes);
  ProgressFile::writeAtomic(cacheDir_, bytes, sizeof(bytes));
  lastSaveMs_ = millis();
}

void AudiobookService::tick() {
  if (!open_) return;
  // A scan takes the module away from either output; waiting through it would
  // end in an output error.
  if ((playing_ || waitingForOutput_) && halAudio.link() == HalAudio::Link::Scanning) pause();
  const bool headphones = halAudio.output() == HalAudio::Output::Headphones;
  if (headphones != shownHeadphones_) {
    shownHeadphones_ = headphones;
    revision_++;  // screens show the output, even of a paused book
  }
  if (waitingForOutput_ && headphones != waitHeadphones_) {
    // A switch made while waiting gets the full wait for the new output.
    waitHeadphones_ = headphones;
    waitStartedMs_ = millis();
  }
  if (waitingForOutput_) {
    const bool linkGone = headphones && !halAudio.waking() &&
                          (halAudio.link() == HalAudio::Link::Failed || halAudio.link() == HalAudio::Link::None);
    if (outputReady()) {
      waitingForOutput_ = false;
      revision_++;
      startPlayback();
    } else if (halAudio.wakeFailed() || linkGone || millis() - waitStartedMs_ > LINK_WAIT_MS) {
      // No clock after every step, failed, or nothing to connect to.
      halAudio.cancelWake();
      waitingForOutput_ = false;
      revision_++;
      reportProblem(PlaybackProblem::Output, generation_);
    } else if (outputSetUp()) {
      halAudio.wakeOutput();  // checks the clock first; no-op while it runs
    }
  } else if (playing_ && outputLost()) {
    // The output changed while playing, or the headset link went away: hold
    // here until it is ready rather than play into nothing.
    stopPlayback();
    waitForOutput();
  }

  const PlaybackProblem problem = currentProblem();
  if (problem != shownProblem_) {
    shownProblem_ = problem;
    revision_++;
  }
  // Checked every pass, not only on change: a retry can fail the same way.
  if (problem != PlaybackProblem::None && (playing_ || waitingForOutput_)) {
    LOG_ERR("ABK", "Paused: %s", problem == PlaybackProblem::Track ? "track unreadable" : "no audio output");
    if (problem == PlaybackProblem::Output) {
      halAudio.note(playing_ ? "playback: no audio clock" : "playback: output not ready");
    }
    pause();
  }

  if (endedGeneration_.load() == generation_.load()) {
    endedGeneration_ = UINT32_MAX;
    if (track_ + 1u < trackCount()) {
      loadTrack(track_ + 1, 0);
    } else {
      // Finished: the next play (or open) starts again from the beginning.
      finished_ = true;
      pause();
      savePosition();
    }
  }

  const size_t chapter = currentChapter();
  if (chapter != shownChapter_) {
    shownChapter_ = chapter;
    revision_++;
    savePosition();
  } else if (playing_ && millis() - lastSaveMs_ >= SAVE_INTERVAL_MS) {
    savePosition();
  }
}

// ---------------------------------------------------------------------------
// Reader task: SD -> ring

void AudiobookService::readerEntry(void* self) { static_cast<AudiobookService*>(self)->readerLoop(); }

bool AudiobookService::sendMessage(const size_t len) {
  // Waits for ring space, but gives up when a newer command arrives.
  while (!quit_) {
    if (xMessageBufferSend(ring_, readerPacket_.get(), len, WAIT_TICKS) == len) return true;
    if (uxQueueMessagesWaiting(commands_) > 0) return false;
  }
  return false;
}

void AudiobookService::readerLoop() {
  {
    // Scoped so the destructors run before vTaskDelete, which never returns.
    HalFileSource source;
    CachedReader cache;
    std::unique_ptr<TrackReader> reader;
    AacConfig aac;
    uint16_t openTrack = UINT16_MAX;
    uint32_t generation = 0;
    bool streaming = false;
    const bool cacheReady = cache.init(&source, READ_CACHE_BYTES);
    if (!cacheReady) LOG_ERR("ABK", "OOM: read cache (%u B)", static_cast<unsigned>(READ_CACHE_BYTES));
    MessageHeader header{};

    while (!quit_) {
      ReaderCommand command;
      if (xQueueReceive(commands_, &command, streaming ? 0 : IDLE_WAIT_TICKS) == pdTRUE) {
        streaming = false;
        generation = command.generation;
        if (command.track != openTrack || !reader) {
          reader.reset();
          source.close();
          cache.invalidate();
          openTrack = UINT16_MAX;
          const std::string path = trackPath(command.track);
          bool opened = cacheReady && source.open(path.c_str());
          if (opened && isMp4Book(path)) {
            auto mp4 = makeUniqueNoThrow<Mp4Reader>();
            opened = mp4 && mp4->open(source, cache, false);
            if (opened) aac = mp4->aacConfig();
            reader = std::move(mp4);
          } else if (opened) {
            auto mp3 = makeUniqueNoThrow<Mp3Reader>();
            opened = mp3 && mp3->open(cache);
            reader = std::move(mp3);
          }
          if (!opened) {
            LOG_ERR("ABK", "Cannot open track %s", path.c_str());
            reader.reset();
            reportProblem(PlaybackProblem::Track, generation);
            continue;
          }
          openTrack = command.track;
          trackDurationMs_ = static_cast<uint32_t>(reader->durationMs());
        }
        uint64_t actual = 0;
        if (!reader->seekMs(command.positionMs, actual)) reader->seekMs(0, actual);
        header = MessageHeader{MessageType::TrackStart, reader->codec(), openTrack, generation, actual, aac};
        memcpy(readerPacket_.get(), &header, sizeof(header));
        streaming = sendMessage(sizeof(header));
        continue;
      }
      if (!streaming) continue;

      uint64_t startMs = 0;
      const int n = reader->nextPacket(readerPacket_.get() + sizeof(header), MAX_PACKET_BYTES, startMs);
      if (n < 0) {
        // A read error is not the end of the book. It goes through the ring so
        // the audio read before it still plays; the position is kept.
        LOG_ERR("ABK", "Read error in track %u", openTrack);
        header = MessageHeader{MessageType::TrackError, reader->codec(), openTrack, generation, startMs, aac};
        memcpy(readerPacket_.get(), &header, sizeof(header));
        sendMessage(sizeof(header));
        streaming = false;
        continue;
      }
      if (n == 0) {
        header = MessageHeader{MessageType::TrackEnd, reader->codec(), openTrack, generation, startMs, aac};
        memcpy(readerPacket_.get(), &header, sizeof(header));
        sendMessage(sizeof(header));
        streaming = false;
        continue;
      }
      header = MessageHeader{MessageType::Packet, reader->codec(), openTrack, generation, startMs, aac};
      memcpy(readerPacket_.get(), &header, sizeof(header));
      if (!sendMessage(sizeof(header) + static_cast<size_t>(n))) streaming = false;
    }
    LOG_DBG("ABK", "Reader stack high-water: %u B", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  readerAlive_ = false;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Decoder task: ring -> PCM -> HalAudio

void AudiobookService::decoderEntry(void* self) { static_cast<AudiobookService*>(self)->decoderLoop(); }

bool AudiobookService::drainPipeline(PcmPipeline& pipeline, const uint32_t generation) {
  while (!quit_ && playing_ && generation == generation_.load()) {
    const size_t n = pipeline.read(mono_.get(), MONO_CHUNK);
    if (n == 0) return true;
    // A short write means the module stopped clocking: stop here and let the
    // caller count the stall instead of blocking on every chunk.
    if (halAudio.write(mono_.get(), n, volume_.load()) < n) return false;
  }
  return false;
}

void AudiobookService::decoderLoop() {
  {
    // Scoped so the destructors run before vTaskDelete, which never returns.
    AudioDecoder decoder;
    PcmPipeline pipeline;
    bool ready = false;
    bool pending = false;
    Codec activeCodec = Codec::Mp3;
    uint16_t activeTrack = UINT16_MAX;
    uint8_t appliedSpeed = speedIndex_.load();
    uint32_t stalledMs = 0;
    MessageHeader header{};
    size_t payloadLen = 0;
    const uint8_t* payload = decoderPacket_.get() + sizeof(MessageHeader);
    TickType_t lastYield = xTaskGetTickCount();

    while (!quit_) {
      if (xTaskGetTickCount() - lastYield >= YIELD_EVERY_TICKS) {
        vTaskDelay(1);
        lastYield = xTaskGetTickCount();
      }
      if (!pending) {
        decoderBusy_ = false;
        const size_t n =
            xMessageBufferReceive(ring_, decoderPacket_.get(), sizeof(MessageHeader) + MAX_PACKET_BYTES, WAIT_TICKS);
        if (n < sizeof(MessageHeader)) continue;
        memcpy(&header, decoderPacket_.get(), sizeof(header));
        payloadLen = n - sizeof(header);
        pending = true;
      }
      // Packets from before a seek or track change are dropped, even while paused.
      if (header.generation != generation_.load()) {
        pending = false;
        continue;
      }
      // Busy is raised before playing_ is read, and pause() clears playing_
      // before reading busy, so one of them always sees the other.
      decoderBusy_ = true;
      if (!playing_) {
        decoderBusy_ = false;
        ulTaskNotifyTake(pdTRUE, WAIT_TICKS);
        continue;
      }
      pending = false;

      switch (header.type) {
        case MessageType::TrackStart:
          if (ready && header.track == activeTrack && header.codec == activeCodec) {
            ready = decoder.flush();
          } else {
            ready = decoder.begin(header.codec, header.aac);
            activeTrack = header.track;
            activeCodec = header.codec;
          }
          if (!ready) {
            LOG_ERR("ABK", "OOM: decoder for track %u", header.track);
            reportProblem(PlaybackProblem::Track, header.generation);
          }
          pipeline.reset();
          stalledMs = 0;
          positionMs_ = static_cast<uint32_t>(header.startMs);
          break;

        case MessageType::Packet: {
          if (!ready) break;
          PcmFormat format;
          const int frames = decoder.decode(payload, payloadLen, pcm_.get(), MAX_DECODED_SAMPLES, format);
          if (frames <= 0) {
            positionMs_ = static_cast<uint32_t>(header.startMs);  // damaged or warm-up frame: skip
            break;
          }
          const uint8_t speed = speedIndex_.load();
          if (pipeline.sourceRate() != format.sampleRate) {
            if (!pipeline.configure(format.sampleRate, playbackSpeed(speed))) {
              LOG_ERR("ABK", "OOM: speed pipeline at %u Hz", static_cast<unsigned>(format.sampleRate));
              reportProblem(PlaybackProblem::Track, header.generation);
              break;
            }
            appliedSpeed = speed;
          } else if (speed != appliedSpeed) {
            pipeline.setSpeed(playbackSpeed(speed));
            appliedSpeed = speed;
          }
          pipeline.write(pcm_.get(), static_cast<size_t>(frames), format.channels);
          if (drainPipeline(pipeline, header.generation)) {
            // The position moves only once the audio has gone out.
            positionMs_ = static_cast<uint32_t>(header.startMs);
            stalledMs = 0;
          } else if (playing_ && header.generation == generation_.load()) {
            stalledMs += WRITE_TIMEOUT_MS;
            if (stalledMs >= OUTPUT_STALL_MS) reportProblem(PlaybackProblem::Output, header.generation);
          }
          break;
        }

        case MessageType::TrackEnd:
          pipeline.finish();
          drainPipeline(pipeline, header.generation);
          endedGeneration_ = header.generation;
          break;

        case MessageType::TrackError:
          pipeline.finish();
          drainPipeline(pipeline, header.generation);
          reportProblem(PlaybackProblem::Track, header.generation);
          break;
      }
    }
    decoder.end();
    LOG_DBG("ABK", "Decoder stack high-water: %u B", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  decoderBusy_ = false;
  decoderAlive_ = false;
  vTaskDelete(nullptr);
}

}  // namespace audiobook

#endif
