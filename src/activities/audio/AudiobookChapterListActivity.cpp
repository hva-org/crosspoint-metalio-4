#include "AudiobookChapterListActivity.h"

#if CROSSPOINT_AUDIOBOOK

#include <I18n.h>

#include "I18nKeys.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

void AudiobookChapterListActivity::onEnter() {
  UiListActivity::onEnter();
  titles = AUDIOBOOK.chapterTitles();
  moveSelectionTo(static_cast<int>(AUDIOBOOK.snapshot().chapter));
  requestUpdate();
}

int AudiobookChapterListActivity::listCount() const { return static_cast<int>(titles.size()); }

const char* AudiobookChapterListActivity::headerTitle() const { return tr(STR_AUDIOBOOK_CHAPTERS); }

void AudiobookChapterListActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  AUDIOBOOK.jumpToChapter(static_cast<size_t>(index));
  finish();
}

void AudiobookChapterListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.rowProvider = [](void* ctx, const uint16_t index, fui::ListItem& item) {
    item.label = static_cast<AudiobookChapterListActivity*>(ctx)->titles[index].text;
    item.actionValue = static_cast<int16_t>(index);
  };
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif
