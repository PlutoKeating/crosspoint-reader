#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

#include "activities/Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"
#include "util/LanguagePacks.h"

class MappedInputManager;

/**
 * Selects the UI language: the built-in catalogue or an SD-card language pack.
 */
class LanguageSelectActivity final : public Activity {
 public:
  explicit LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageSelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();

  void onBack() { finish(); }
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<language_packs::PackInfo> languages;
  int itemCount() const { return static_cast<int>(languages.size()); }
};
