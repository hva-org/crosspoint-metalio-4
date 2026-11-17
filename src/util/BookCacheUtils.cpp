#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Xtc.h>

#include "ClippingStore.h"
#if CROSSPOINT_AUDIOBOOK
#include <BookModel.h>
#include <HalStorage.h>

#include "audio/AudiobookService.h"
#endif

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

#if CROSSPOINT_AUDIOBOOK
  constexpr char AUDIO_PREFIX[] = "audio_";
  if (strncmp(name, AUDIO_PREFIX, std::size(AUDIO_PREFIX) - 1) == 0) return true;
#endif
  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
#if CROSSPOINT_AUDIOBOOK
  // Web and WebDAV renames, moves and overwrites, and OPDS and plugin downloads, come through here.
  if (AUDIOBOOK.isAffectedBy(path)) AUDIOBOOK.close();
#endif
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
#if CROSSPOINT_AUDIOBOOK
  } else if (audiobook::isAudiobookFile(path)) {
    // A single-file book's saved position goes with it; a folder book's
    // belongs to the folder, which may still hold other tracks.
    if (audiobook::progressKey(path) == path) Storage.removeDir(audiobook::progressDir(path).c_str());
#endif
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
#if CROSSPOINT_AUDIOBOOK
  if (AUDIOBOOK.isAffectedBy(path)) AUDIOBOOK.close();  // before the file goes
#endif
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  return ClippingStore::deleteForFilePath(path, "epub");
}
