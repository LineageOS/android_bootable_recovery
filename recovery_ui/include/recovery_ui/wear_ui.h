/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RECOVERY_WEAR_UI_H
#define RECOVERY_WEAR_UI_H

#include <string>
#include <vector>

#include "screen_ui.h"

class WearRecoveryUI : public ScreenRecoveryUI {
 public:
  WearRecoveryUI();

  bool Init(const std::string& locale) override;

  void SetStage(int current, int max) override;

 protected:
  // curved progress bar frames for round screens
  std::vector<std::unique_ptr<GRSurface>> progress_frames_;
  std::vector<std::unique_ptr<GRSurface>> rtl_progress_frames_;

  // progress bar vertical position, it's centered horizontally
  const int progress_bar_baseline_;

  // Unusable rows when displaying the recovery menu, including the lines for headers (Android
  // Recovery, build id and etc) and the bottom lines that may otherwise go out of the screen.
  const int menu_unusable_rows_;

  const bool is_screen_circle_;

  std::unique_ptr<Menu> CreateMenu(const std::vector<std::string>& text_headers,
                                   const std::vector<std::string>& text_items,
                                   size_t initial_selection) const override;

  int GetProgressBaseline() const override;

  int GetTextBaseline() const override;

  void update_progress_locked() override;

  void LoadAnimation() override;

  bool IsWearable() override;

  void SetProgress(float fraction) override;

 private:
  void draw_background_locked() override;
  void draw_screen_locked() override;
  void draw_circle_foreground_locked();
  size_t GetProgressFrameIndex(float fraction) const;

};

#endif  // RECOVERY_WEAR_UI_H
