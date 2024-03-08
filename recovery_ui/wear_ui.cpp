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

#include "otautil/paths.h"
#include "recovery_ui/wear_ui.h"

#include <string.h>

#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include <minui/minui.h>

constexpr int kDefaultProgressBarBaseline = 259;
constexpr int kDefaultMenuUnusableRows = 9;
constexpr int kProgressBarVerticalOffsetDp = 72;
constexpr bool kDefaultIsScreenCircle = true;

WearRecoveryUI::WearRecoveryUI()
    : ScreenRecoveryUI(),
      progress_bar_baseline_(android::base::GetIntProperty("ro.recovery.ui.progress_bar_baseline",
                                                           kDefaultProgressBarBaseline)),
      menu_unusable_rows_(android::base::GetIntProperty("ro.recovery.ui.menu_unusable_rows",
                                                        kDefaultMenuUnusableRows)),
      is_screen_circle_(android::base::GetBoolProperty("ro.recovery.ui.is_screen_circle",
                                                             kDefaultIsScreenCircle)) {
  // TODO: menu_unusable_rows_ should be computed based on the lines in draw_screen_locked().
  touch_screen_allowed_ = true;
}

static void FlipOrientation() {
  auto rotation = gr_get_rotation();
  if (rotation == GRRotation::NONE) {
    gr_rotate(GRRotation::DOWN);
  } else if (rotation == GRRotation::DOWN) {
    gr_rotate(GRRotation::NONE);
  } else {
    LOG(WARNING) << "Unsupported rotation for wrist orientation" << static_cast<int>(rotation);
  }
}

// Match values in
// frameworks/opt/wear/src/com/android/clockwork/wristorientation/WristOrientationService.java
enum class WristOrientation : unsigned {
  LEFT_WRIST_ROTATION_0 = 0,
  LEFT_WRIST_ROTATION_180 = 1,
  RIGHT_WRIST_ROTATION_0 = 2,
  RIGHT_WRIST_ROTATION_180 = 3,
};

static void InitWristOrientation() {
  auto prop = android::base::GetUintProperty("ro.boot.wrist_orientation", 0u);
  WristOrientation orientation{ prop };
  if (orientation == WristOrientation::LEFT_WRIST_ROTATION_180 ||
      orientation == WristOrientation::RIGHT_WRIST_ROTATION_180) {
    LOG(INFO)
        << "InitWristOrientation(): flipping orientation because, 'ro.boot.wrist_orientation'="
        << prop;

    FlipOrientation();
  }
}

bool WearRecoveryUI::Init(const std::string& locale) {
  auto result = ScreenRecoveryUI::Init(locale);
  auto wrist_orientation_enabled =
      android::base::GetBoolProperty("config.enable_wristorientation", false);
  LOG(INFO) << "WearRecoveryUI::Init(): enable_wristorientation=" << wrist_orientation_enabled;
  if (wrist_orientation_enabled) {
    InitWristOrientation();
  }
  return result;
}

// Draw background frame on the screen.  Does not flip pages.
// Should only be called with updateMutex locked.
// TODO merge drawing routines with screen_ui
void WearRecoveryUI::draw_background_locked() {
  pagesIdentical = false;
  gr_color(0, 0, 0, 255);
  gr_fill(0, 0, gr_fb_width(), gr_fb_height());

  if (current_icon_ == ERROR) {
    const auto& frame = GetCurrentFrame();
    int frame_width = gr_get_width(frame);
    int frame_height = gr_get_height(frame);
    int frame_x = (gr_fb_width() - frame_width) / 2;
    int frame_y = (gr_fb_height() - frame_height) / 2;
    gr_blit(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
  }

  if (current_icon_ != NONE) {
    // Draw recovery text on screen centered
    const auto& text = GetCurrentText();
    int text_x = (ScreenWidth() - gr_get_width(text)) / 2;
    int text_y = (ScreenHeight() - gr_get_height(text)) / 2;
    gr_color(255, 255, 255, 255);
    gr_texticon(text_x, text_y, text);
  }
}

void WearRecoveryUI::draw_screen_locked() {
  if (!show_text) {
    draw_background_locked();
    if (is_screen_circle_) {
        draw_circle_foreground_locked();
    } else {
        draw_foreground_locked();
    }
    return;
  }

  SetColor(UIElement::TEXT_FILL);
  gr_clear();

  // clang-format off
  static std::vector<std::string> SWIPE_HELP = {
    "Swipe up/down to move.",
    "Swipe left/right to select.",
    "",
  };
  // clang-format on
  draw_menu_and_text_buffer_locked(SWIPE_HELP);
}

void WearRecoveryUI::draw_circle_foreground_locked() {
    if (current_icon_ != NONE) {
        const auto& frame = GetCurrentFrame();
        int frame_width = gr_get_width(frame);
        int frame_height = gr_get_height(frame);
        int frame_x = (ScreenWidth() - frame_width) / 2;
        int frame_y = GetAnimationBaseline();
        DrawSurface(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
      }

    if (progressBarType == DETERMINATE) {
        const auto& first_progress_frame = rtl_locale_ ? rtl_progress_frames_[0].get()
                                                        :progress_frames_[0].get();
        int width = gr_get_width(first_progress_frame);
        int height = gr_get_height(first_progress_frame);

        int progress_x = (ScreenWidth() - width) / 2;
        int progress_y = GetProgressBaseline();

        const auto index = GetProgressFrameIndex(progress);
        const auto& frame = rtl_locale_ ? rtl_progress_frames_[index].get()
                                        : progress_frames_[index].get();

        DrawSurface(frame, 0, 0, width, height, progress_x, progress_y);
    }
}

void WearRecoveryUI::LoadAnimation() {
  ScreenRecoveryUI::LoadAnimation();
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(Paths::Get().resource_dir().c_str()),
                                                closedir);
  dirent* de;
  std::vector<std::string> progress_frame_names;
  std::vector<std::string> rtl_progress_frame_names;

  if(dir.get() == nullptr) abort();

  while ((de = readdir(dir.get())) != nullptr) {
    int value, num_chars;
    if (sscanf(de->d_name, "progress%d%n.png", &value, &num_chars) == 1) {
      progress_frame_names.emplace_back(de->d_name, num_chars);
    } else if (sscanf(de->d_name, "rtl_progress%d%n.png", &value, &num_chars) == 1) {
      rtl_progress_frame_names.emplace_back(de->d_name, num_chars);
    }
  }

  size_t progress_frames = progress_frame_names.size();
  size_t rtl_progress_frames = rtl_progress_frame_names.size();

  // You must have an animation.
  if (progress_frames == 0 || rtl_progress_frames == 0) abort();

  std::sort(progress_frame_names.begin(), progress_frame_names.end());
  std::sort(rtl_progress_frame_names.begin(), rtl_progress_frame_names.end());

  progress_frames_.clear();
  progress_frames_.reserve(progress_frames);
  for (const auto& frame_name : progress_frame_names) {
    progress_frames_.emplace_back(LoadBitmap(frame_name));
  }

  rtl_progress_frames_.clear();
    rtl_progress_frames_.reserve(rtl_progress_frames);
    for (const auto& frame_name : rtl_progress_frame_names) {
      rtl_progress_frames_.emplace_back(LoadBitmap(frame_name));
  }
}

void WearRecoveryUI::SetProgress(float fraction) {
    if (is_screen_circle_) {
       std::lock_guard<std::mutex> lg(updateMutex);
       if (fraction < 0.0) fraction = 0.0;
       if (fraction > 1.0) fraction = 1.0;
       if (progressBarType == DETERMINATE && fraction > progress) {
          // Skip updates that aren't visibly different.
          if (GetProgressFrameIndex(fraction) != GetProgressFrameIndex(progress)) {
              // circular display
              progress = fraction;
              update_progress_locked();
          }
       }
    } else {
        // rectangular display
        ScreenRecoveryUI::SetProgress(fraction);
    }
}

int WearRecoveryUI::GetProgressBaseline() const {
  int progress_height = gr_get_height(progress_frames_[0].get());
  return (ScreenHeight() - progress_height) / 2 + PixelsFromDp(kProgressBarVerticalOffsetDp);
}

int WearRecoveryUI::GetTextBaseline() const {
  if (is_screen_circle_) {
        return GetProgressBaseline() - PixelsFromDp(kProgressBarVerticalOffsetDp) -
                                    gr_get_height(installing_text_.get());
  } else {
       return ScreenRecoveryUI::GetTextBaseline();
  }
}

size_t WearRecoveryUI::GetProgressFrameIndex(float fraction) const {
  return static_cast<size_t>(fraction * (progress_frames_.size() - 1));
}

// TODO merge drawing routines with screen_ui
void WearRecoveryUI::update_progress_locked() {
  draw_screen_locked();
  gr_flip();
}

bool WearRecoveryUI::IsWearable() {
  return true;
}

void WearRecoveryUI::SetStage(int /* current */, int /* max */) {}

std::unique_ptr<Menu> WearRecoveryUI::CreateMenu(const std::vector<std::string>& text_headers,
                                                 const std::vector<std::string>& text_items,
                                                 size_t initial_selection) const {
  if (text_rows_ > 0 && text_cols_ > 0) {
    return std::make_unique<TextMenu>(false, text_cols_ - 1, text_headers, text_items,
                                      initial_selection, char_height_, *this);
  }

  return nullptr;
}
