/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
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

#include "screen_ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <minui/minui.h>

#include <healthd/BatteryMonitor.h>

#include "common.h"
#include "device.h"
#include "ui.h"

// Return the current time as a double (including fractions of a second).
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void get_battery_status(bool& charged, int& capacity) {
  struct healthd_config healthd_config = {
    .batteryStatusPath = android::String8(android::String8::kEmptyString),
    .batteryHealthPath = android::String8(android::String8::kEmptyString),
    .batteryPresentPath = android::String8(android::String8::kEmptyString),
    .batteryCapacityPath = android::String8(android::String8::kEmptyString),
    .batteryVoltagePath = android::String8(android::String8::kEmptyString),
    .batteryTemperaturePath = android::String8(android::String8::kEmptyString),
    .batteryTechnologyPath = android::String8(android::String8::kEmptyString),
    .batteryCurrentNowPath = android::String8(android::String8::kEmptyString),
    .batteryCurrentAvgPath = android::String8(android::String8::kEmptyString),
    .batteryChargeCounterPath = android::String8(android::String8::kEmptyString),
    .batteryFullChargePath = android::String8(android::String8::kEmptyString),
    .batteryCycleCountPath = android::String8(android::String8::kEmptyString),
    .energyCounter = NULL,
    .boot_min_cap = 0,
    .screen_on = NULL
  };
  healthd_board_init(&healthd_config);

  android::BatteryMonitor monitor;
  monitor.init(&healthd_config);

  int charge_status = monitor.getChargeStatus();
  // Treat unknown status as charged.
  charged = (charge_status != android::BATTERY_STATUS_DISCHARGING &&
             charge_status != android::BATTERY_STATUS_NOT_CHARGING);
  android::BatteryProperty prop;
  android::status_t status = monitor.getProperty(android::BATTERY_PROP_CAPACITY, &prop);
  // If we can't read battery percentage, it may be a device without battery. In this
  // situation, use 100 as a fake battery percentage.
  if (status != 0) {
    prop.valueInt64 = 100;
  }
  capacity = (int)prop.valueInt64;
}

ScreenMenuItem::~ScreenMenuItem() {
  if (icon_) {
    res_free_surface(icon_);
  }
  if (icon_sel_) {
    res_free_surface(icon_sel_);
  }
}

GRSurface* ScreenMenuItem::icon() {
  if (!icon_) {
    res_create_display_surface(icon_name_.c_str(), &icon_);
  }
  return icon_;
}

GRSurface* ScreenMenuItem::icon_sel() {
  if (icon_name_sel_.empty()) {
    return icon();
  }
  if (!icon_sel_) {
    res_create_display_surface(icon_name_sel_.c_str(), &icon_sel_);
  }
  return icon_sel_;
}

ScreenRecoveryUI::ScreenRecoveryUI()
    : kMarginWidth(RECOVERY_UI_MARGIN_WIDTH),
      kMarginHeight(RECOVERY_UI_MARGIN_HEIGHT),
      kAnimationFps(RECOVERY_UI_ANIMATION_FPS),
      kDensity(static_cast<float>(android::base::GetIntProperty("ro.sf.lcd_density", 160)) / 160.f),
      currentIcon(NONE),
      progressBarType(EMPTY),
      progressScopeStart(0),
      progressScopeSize(0),
      progress(0),
      pagesIdentical(false),
      text_cols_(0),
      text_rows_(0),
      text_(nullptr),
      text_col_(0),
      text_row_(0),
      show_text(false),
      show_text_ever(false),
      previous_row_ended(false),
      update_screen_on_print(false),
      menu_is_main_(true),
      menu_type_(MT_NONE),
      menu_headers_(nullptr),
      menu_start_y_(0),
      show_menu(false),
      menu_show_start(0),
      menu_show_count(0),
      menu_sel(0),
      file_viewer_text_(nullptr),
      intro_frames(0),
      loop_frames(0),
      current_frame(0),
      intro_done(false),
      stage(-1),
      max_stage(-1),
      locale_(""),
      rtl_locale_(false),
      updateMutex(PTHREAD_MUTEX_INITIALIZER) {}

GRSurface* ScreenRecoveryUI::GetCurrentFrame() const {
  if (currentIcon == INSTALLING_UPDATE || currentIcon == ERASING) {
    return intro_done ? loopFrames[current_frame] : introFrames[current_frame];
  }
  return nullptr;
}

GRSurface* ScreenRecoveryUI::GetCurrentText() const {
  switch (currentIcon) {
    case ERASING:
      return erasing_text;
    case ERROR:
      return error_text;
    case INSTALLING_UPDATE:
      return installing_text;
    case NO_COMMAND:
      return no_command_text;
    case NONE:
      abort();
  }
}

int ScreenRecoveryUI::PixelsFromDp(int dp) const {
  return dp * kDensity;
}

// Here's the intended layout:

//          | portrait    large        landscape      large
// ---------+-------------------------------------------------
//      gap |
// icon     |                   (200dp)
//      gap |    68dp      68dp             56dp      112dp
// text     |                    (14sp)
//      gap |    32dp      32dp             26dp       52dp
// progress |                     (2dp)
//      gap |

// Note that "baseline" is actually the *top* of each icon (because that's how our drawing routines
// work), so that's the more useful measurement for calling code. We use even top and bottom gaps.

enum Layout { PORTRAIT = 0, PORTRAIT_LARGE = 1, LANDSCAPE = 2, LANDSCAPE_LARGE = 3, LAYOUT_MAX };
enum Dimension { TEXT = 0, ICON = 1, DIMENSION_MAX };
static constexpr int kLayouts[LAYOUT_MAX][DIMENSION_MAX] = {
  { 32,  68, },  // PORTRAIT
  { 32,  68, },  // PORTRAIT_LARGE
  { 26,  56, },  // LANDSCAPE
  { 52, 112, },  // LANDSCAPE_LARGE
};

int ScreenRecoveryUI::GetAnimationBaseline() const {
  return GetTextBaseline() - PixelsFromDp(kLayouts[layout_][ICON]) - gr_get_height(loopFrames[0]);
}

int ScreenRecoveryUI::GetTextBaseline() const {
  return GetProgressBaseline() - PixelsFromDp(kLayouts[layout_][TEXT]) -
         gr_get_height(installing_text);
}

int ScreenRecoveryUI::GetProgressBaseline() const {
  int elements_sum = gr_get_height(loopFrames[0]) + PixelsFromDp(kLayouts[layout_][ICON]) +
                     gr_get_height(installing_text) + PixelsFromDp(kLayouts[layout_][TEXT]) +
                     gr_get_height(progressBarFill);
  int bottom_gap = (ScreenHeight() - elements_sum) / 2;
  return ScreenHeight() - bottom_gap - gr_get_height(progressBarFill);
}

// Draw the currently selected stage icon(s) (if any).
// Does not flip pages. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_background_locked() {
  if (currentIcon != NONE && currentIcon != NO_COMMAND) {
    if (max_stage != -1) {
      int stage_height = gr_get_height(stageMarkerEmpty);
      int stage_width = gr_get_width(stageMarkerEmpty);
      int stage_x = kMarginWidth + (ScreenWidth() - max_stage * gr_get_width(stageMarkerEmpty)) / 2;
      int stage_y = kMarginHeight;
      for (int i = 0; i < max_stage; ++i) {
        GRSurface* stage_surface = (i < stage) ? stageMarkerFill : stageMarkerEmpty;
        DrawSurface(stage_surface, 0, 0, stage_width, stage_height, stage_x, stage_y);
        stage_x += stage_width;
      }
    }
  }
}

// Draws either the animation and progress bar or the currently
// selected icon and text on the screen.
// Does not flip pages. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_foreground_locked(int& y) {
  GRSurface* frame = GetCurrentFrame();
  if (frame) {
    // Show animation frame and progress bar
    int frame_width = gr_get_width(frame);
    int frame_height = gr_get_height(frame);
    int frame_x = kMarginWidth + (ScreenWidth() - frame_width) / 2;
    int frame_y = kMarginHeight + GetAnimationBaseline();
    DrawSurface(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
    y = frame_y + frame_height;

    if (progressBarType != EMPTY) {
      int width = gr_get_width(progressBarEmpty);
      int height = gr_get_height(progressBarEmpty);

      int progress_x = kMarginWidth + (ScreenWidth() - width) / 2;
      int progress_y = kMarginHeight + GetProgressBaseline();

      // Erase behind the progress bar (in case this was a progress-only update)
      gr_color(0, 0, 0, 255);
      DrawFill(progress_x, progress_y, width, height);

      if (progressBarType == DETERMINATE) {
        float p = progressScopeStart + progress * progressScopeSize;
        int pos = static_cast<int>(p * width);

        if (rtl_locale_) {
          // Fill the progress bar from right to left.
          if (pos > 0) {
            DrawSurface(progressBarFill, width - pos, 0, pos, height, progress_x + width - pos,
                        progress_y);
          }
          if (pos < width - 1) {
            DrawSurface(progressBarEmpty, 0, 0, width - pos, height, progress_x, progress_y);
          }
        } else {
          // Fill the progress bar from left to right.
          if (pos > 0) {
            DrawSurface(progressBarFill, 0, 0, pos, height, progress_x, progress_y);
          }
          if (pos < width - 1) {
            DrawSurface(progressBarEmpty, pos, 0, width - pos, height, progress_x + pos,
                        progress_y);
          }
        }
      }
      y = progress_y + height;
    }
  } else {
    // Show icon and text
    if (currentIcon != NONE && currentIcon != NO_COMMAND) {
      GRSurface* icon_surface = error_icon;
      int icon_width = gr_get_width(icon_surface);
      int icon_height = gr_get_height(icon_surface);
      int icon_x = kMarginWidth + (gr_fb_width() - icon_width) / 2;
      int icon_y = kMarginHeight + GetAnimationBaseline();
      gr_blit(icon_surface, 0, 0, icon_width, icon_height, icon_x, icon_y);

      GRSurface* text_surface = GetCurrentText();
      int text_width = gr_get_width(text_surface);
      int text_height = gr_get_height(text_surface);
      int text_x = kMarginWidth + (gr_fb_width() - text_width) / 2;
      int text_y = kMarginHeight + GetTextBaseline();
      gr_color(255, 255, 255, 255);
      gr_texticon(text_x, text_y, text_surface);

      y = text_y + text_height;
    }
  }
}

/* Lineage teal: #167c80 */
void ScreenRecoveryUI::SetColor(UIElement e) const {
  switch (e) {
    case STATUSBAR:
      gr_color(255, 255, 255, 255);
      break;
    case INFO:
      gr_color(249, 194, 0, 255);
      break;
    case HEADER:
      gr_color(247, 0, 6, 255);
      break;
    case MENU:
    case MENU_SEL_BG:
      gr_color(0xd8, 0xd8, 0xd8, 255);
      break;
    case MENU_SEL_BG_ACTIVE:
      gr_color(138, 135, 134, 255);
      break;
    case MENU_SEL_FG:
      gr_color(0x16, 0x7c, 0x80, 255);
      break;
    case LOG:
      gr_color(196, 196, 196, 255);
      break;
    case TEXT_FILL:
      gr_color(0, 0, 0, 160);
      break;
    default:
      gr_color(255, 255, 255, 255);
      break;
  }
}

void ScreenRecoveryUI::SelectAndShowBackgroundText(const std::vector<std::string>& locales_entries,
                                                   size_t sel) {
  SetLocale(locales_entries[sel]);
  std::vector<std::string> text_name = { "erasing_text", "error_text", "installing_text",
                                         "installing_security_text", "no_command_text" };
  std::unordered_map<std::string, std::unique_ptr<GRSurface, decltype(&free)>> surfaces;
  for (const auto& name : text_name) {
    GRSurface* text_image = nullptr;
    LoadLocalizedBitmap(name.c_str(), &text_image);
    if (!text_image) {
      Print("Failed to load %s\n", name.c_str());
      return;
    }
    surfaces.emplace(name, std::unique_ptr<GRSurface, decltype(&free)>(text_image, &free));
  }

  pthread_mutex_lock(&updateMutex);
  gr_color(0, 0, 0, 255);
  gr_clear();

  int text_y = kMarginHeight;
  int text_x = kMarginWidth;
  int line_spacing = gr_sys_font()->char_height;  // Put some extra space between images.
  // Write the header and descriptive texts.
  SetColor(INFO);
  std::string header = "Show background text image";
  text_y += DrawTextLine(text_x, text_y, header.c_str(), true);
  std::string locale_selection = android::base::StringPrintf(
      "Current locale: %s, %zu/%zu", locales_entries[sel].c_str(), sel, locales_entries.size());
  const char* instruction[] = { locale_selection.c_str(),
                                "Use volume up/down to switch locales and power to exit.",
                                nullptr };
  text_y += DrawWrappedTextLines(text_x, text_y, instruction);

  // Iterate through the text images and display them in order for the current locale.
  for (const auto& p : surfaces) {
    text_y += line_spacing;
    SetColor(LOG);
    text_y += DrawTextLine(text_x, text_y, p.first.c_str(), false);
    gr_color(255, 255, 255, 255);
    gr_texticon(text_x, text_y, p.second.get());
    text_y += gr_get_height(p.second.get());
  }
  // Update the whole screen.
  gr_flip();
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::CheckBackgroundTextImages(const std::string& saved_locale) {
  // Load a list of locales embedded in one of the resource files.
  std::vector<std::string> locales_entries = get_locales_in_png("installing_text");
  if (locales_entries.empty()) {
    Print("Failed to load locales from the resource files\n");
    return;
  }
  size_t selected = 0;
  SelectAndShowBackgroundText(locales_entries, selected);

  FlushKeys();
  while (true) {
    RecoveryUI::InputEvent evt = WaitInputEvent();
    if (evt.type() != RecoveryUI::EVENT_TYPE_KEY) {
      break;
    }
    if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER) {
      break;
    } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP) {
      selected = (selected == 0) ? locales_entries.size() - 1 : selected - 1;
      SelectAndShowBackgroundText(locales_entries, selected);
    } else if (evt.key() == KEY_DOWN || evt.key() == KEY_VOLUMEDOWN) {
      selected = (selected == locales_entries.size() - 1) ? 0 : selected + 1;
      SelectAndShowBackgroundText(locales_entries, selected);
    }
  }

  SetLocale(saved_locale);
}

int ScreenRecoveryUI::ScreenWidth() const {
  return gr_fb_width();
}

int ScreenRecoveryUI::ScreenHeight() const {
  return gr_fb_height();
}

void ScreenRecoveryUI::DrawSurface(GRSurface* surface, int sx, int sy, int w, int h, int dx,
                                   int dy) const {
  gr_blit(surface, sx, sy, w, h, dx, dy);
}

int ScreenRecoveryUI::DrawHorizontalRule(int y) const {
  gr_fill(0, y + 4, ScreenWidth(), y + 6);
  return 8;
}

void ScreenRecoveryUI::DrawHighlightBar(int x, int y, int width, int height) const {
  gr_fill(x, y, x + width, y + height);
}

void ScreenRecoveryUI::DrawFill(int x, int y, int w, int h) const {
  gr_fill(x, y, w, h);
}

void ScreenRecoveryUI::DrawTextIcon(int x, int y, GRSurface* surface) const {
  gr_texticon(x, y, surface);
}

int ScreenRecoveryUI::DrawTextLine(int x, int y, const char* line, bool bold) const {
  gr_text(gr_sys_font(), x, y, line, bold);
  return char_height_ + 4;
}

int ScreenRecoveryUI::DrawTextLines(int x, int y, const char* const* lines) const {
  int offset = 0;
  for (size_t i = 0; lines != nullptr && lines[i] != nullptr; ++i) {
    offset += DrawTextLine(x, y + offset, lines[i], false);
  }
  return offset;
}

int ScreenRecoveryUI::DrawWrappedTextLines(int x, int y, const char* const* lines) const {
  int offset = 0;
  for (size_t i = 0; lines != nullptr && lines[i] != nullptr; ++i) {
    // The line will be wrapped if it exceeds text_cols_.
    std::string line(lines[i]);
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols_ + 1);
      if (sub.size() <= text_cols_) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols_ columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can
          sub.resize(text_cols_);
          next_start += text_cols_;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      gr_text(gr_menu_font(), x, y + offset, sub.c_str(), false);
      offset += menu_char_height_ + 4;
    }
  }
  return offset;
}

void ScreenRecoveryUI::draw_statusbar_locked() {
  int y = kMarginHeight;
  int x;

  int icon_x, icon_y, icon_h, icon_w;

  // Battery status
  bool batt_charged;
  int batt_capacity;
  get_battery_status(batt_charged, batt_capacity);
  char batt_capacity_str[3 + 1 + 1];
  snprintf(batt_capacity_str, sizeof(batt_capacity_str), "%d%%", batt_capacity);

  // Draw status bar from right to left

  // Battery icon
  x = gr_fb_width() - RECOVERY_UI_MARGIN_STATUSBAR;
  x -= 1 * char_width_;
  SetColor((batt_capacity < 20) ? HEADER : STATUSBAR);

  // Top
  icon_x = x + char_width_ / 3;
  icon_y = y;
  icon_w = char_width_ / 3;
  icon_h = char_height_ / 12;
  gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h);

  // Main rect
  icon_x = x;
  icon_y = y + icon_h;
  icon_w = char_width_;
  icon_h = char_height_ - (char_height_ / 12);
  gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h);

  // Capacity
  icon_x = x + char_width_ / 6;
  icon_y = y + char_height_ / 12;
  icon_w = char_width_ - (2 * char_width_ / 6);
  icon_h = char_height_ - (3 * char_height_ / 12);
  int cap_h = icon_h * batt_capacity / 100;
  gr_fill(icon_x, icon_y + icon_h - cap_h, icon_x + icon_w, icon_y + icon_h);
  gr_color(0, 0, 0, 255);
  gr_fill(icon_x, icon_y, icon_x + icon_w, icon_y + icon_h - cap_h);
  SetColor(STATUSBAR);

  x -= char_width_;  // Separator

  // Battery text
  x -= strlen(batt_capacity_str) * char_width_;
  gr_text(gr_sys_font(), x, y, batt_capacity_str, false);
}

/*
 * Header layout:
 *   * 1/32: Status bar
 *   * Header image
 *   * 1/32: Margin
 */
void ScreenRecoveryUI::draw_header_locked(int& y) {
  int h_unit = gr_fb_width() / 9;
  int v_unit = gr_fb_height() / 16;

  GRSurface* icon;
  int icon_x, icon_y, icon_h, icon_w;

  y += v_unit / 2;  // Margin

  // Draw back icon if not in main menu
  if (!menu_is_main_) {
    icon = (menu_sel == -1 ? ic_back_sel : ic_back);
    icon_w = gr_get_width(icon);
    icon_h = gr_get_height(icon);
    icon_x = kMarginWidth + (h_unit / 2) + ((h_unit * 1) - icon_w) / 2;
    icon_y = y + ((v_unit * 1) - icon_h) / 2;
    gr_blit(icon, 0, 0, icon_w, icon_h, icon_x, icon_y);
  }
  y += v_unit;

  // Draw logo
  icon = logo_image;
  icon_w = gr_get_width(icon);
  icon_h = gr_get_height(icon);
  icon_x = kMarginWidth + (gr_fb_width() - icon_w) / 2;
  icon_y = y + ((v_unit * 4) - icon_h) / 2;
  gr_blit(icon, 0, 0, icon_w, icon_h, icon_x, icon_y);
  y += v_unit * 4;

  y += v_unit * 1;  // Margin
}

void ScreenRecoveryUI::draw_text_menu_locked(int& y) {
  static constexpr int kMenuIndent = 4;
  int x = kMarginWidth + kMenuIndent;
  // An item should not be displayed if it's shown height would be less than 75% of its true height
  static const int kMinItemHeight = MenuItemHeight() * 3 / 4;

  draw_statusbar_locked();
  draw_header_locked(y);

  if (menu_headers_) {
    SetColor(HEADER);
    // Ignore kMenuIndent, which is not taken into account by text_cols_.
    y += DrawWrappedTextLines(kMarginWidth, y, menu_headers_);

    SetColor(MENU);
    y += DrawHorizontalRule(y) + 4;
  }

  menu_start_y_ = y;
  int i;
  for (i = menu_show_start; i < (int)menu_items_.size() && y + kMinItemHeight < gr_fb_height();
       ++i) {
    const ScreenMenuItem& item = menu_items_.at(i);
    if (i == menu_sel) {
      SetColor(MENU_SEL_FG);
      y += menu_char_height_;
      gr_text(gr_menu_font(), x, y, item.text().c_str(), false);
      y += menu_char_height_;
      y += menu_char_height_;
    } else {
      SetColor(MENU);
      y += menu_char_height_;
      gr_text(gr_menu_font(), x, y, item.text().c_str(), false);
      y += menu_char_height_;
      y += menu_char_height_;
    }
  }
  menu_show_count = i - menu_show_start;
}

/*
 * Grid layout.
 *
 * Grid item:
 *   Horizontal:
 *     * 3/9 of screen per item.
 *     * 1/9 of screen margin around/between items.
 *   Vertical:
 *     * 3/16 of screen per item.
 *     * No margin between items.
 *
 * Within a grid item:
 *   Asher's icons 1/5 of grid both dimensions.
 *   Current icons 2/5 of grid both dimensions.
 *   Horizontal:
 *     * All items centered.
 *   Vertical:
 *     * Icon lower aligned in top 2/3.
 *     * Text upper aligned in low 1/3 plus half line margin.
 */
void ScreenRecoveryUI::draw_grid_menu_locked(int& y) {
  int h_unit = gr_fb_width() / 9;
  int v_unit = gr_fb_height() / 16;

  int grid_w = h_unit * 3;
  int grid_h = v_unit * 3;

  draw_statusbar_locked();
  draw_header_locked(y);

  menu_start_y_ = y;
  int i;
  for (i = menu_show_start; i < (int)menu_items_.size() && y + grid_h < gr_fb_height(); ++i) {
    ScreenMenuItem& item = menu_items_.at(i);
    int grid_x = kMarginWidth + ((i % 2) ? h_unit * 5 : h_unit * 1);
    int grid_y = y;
    if (item.icon()) {
      GRSurface* icon = (i == menu_sel) ? item.icon_sel() : item.icon();
      int icon_w = gr_get_width(icon);
      int icon_h = gr_get_height(icon);
      int icon_x = grid_x + (grid_w - icon_w) / 2;
      int icon_y = grid_y + ((grid_h * 2 / 3) - icon_h) / 2;
      gr_blit(icon, 0, 0, icon_w, icon_h, icon_x, icon_y);
    }
    if (!item.text().empty()) {
      int text_w = item.text().size() * char_width_;
      int text_x = grid_x + (grid_w - text_w) / 2;
      int text_y = grid_y + (grid_h * 2 / 3) + (char_height_ / 2);
      SetColor(i == menu_sel ? MENU_SEL_FG : MENU);
      gr_text(gr_sys_font(), text_x, text_y, item.text().c_str(), false);
    }
    if (i % 2) {
      y += grid_h;
      grid_y = y;
    }
  }
  menu_show_count = i - menu_show_start;
}

// Redraws everything on the screen. Does not flip pages. Should only be called with updateMutex
// locked.
void ScreenRecoveryUI::draw_screen_locked() {
  pagesIdentical = false;
  gr_color(0, 0, 0, 255);
  gr_clear();

  int y = kMarginHeight;
  if (show_menu) {
    switch (menu_type_) {
      case MT_LIST:
        draw_text_menu_locked(y);
        break;
      case MT_GRID:
        draw_grid_menu_locked(y);
        break;
      default:
        break;
    }

    // Draw version info
    if (menu_is_main_) {
      int text_x, text_y;
      text_x = kMarginWidth + (gr_fb_width() - (android_version_.size() * char_width_)) / 2;
      text_y = gr_fb_height() - 2 * (char_height_ + 4);
      SetColor(MENU);
      DrawTextLine(text_x, text_y, android_version_.c_str(), false);
      text_x = kMarginWidth + (gr_fb_width() - (lineage_version_.size() * char_width_)) / 2;
      text_y = gr_fb_height() - 1 * (char_height_ + 4);
      DrawTextLine(text_x, text_y, lineage_version_.c_str(), false);
    }
  } else {
    draw_background_locked();
    draw_foreground_locked(y);

    if (show_text) {
      // Display from the bottom up, until we hit the top of the screen, the
      // bottom of the foreground, or we've displayed the entire text buffer.
      SetColor(LOG);
      int row = (text_rows_ - 1) % text_rows_;
      size_t count = 0;
      for (int ty = gr_fb_height() - kMarginHeight - char_height_; ty >= y && count < text_rows_;
           ty -= char_height_, ++count) {
        DrawTextLine(kMarginWidth, ty, text_[row], false);
        --row;
        if (row < 0) row = text_rows_ - 1;
      }
    }
  }
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_screen_locked() {
  draw_screen_locked();
  gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_progress_locked() {
  if (!pagesIdentical) {
    draw_screen_locked();
    pagesIdentical = true;
  } else {
    int y = kMarginHeight;
    draw_foreground_locked(y);
  }
  gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
void* ScreenRecoveryUI::ProgressThreadStartRoutine(void* data) {
  reinterpret_cast<ScreenRecoveryUI*>(data)->ProgressThreadLoop();
  return nullptr;
}

void ScreenRecoveryUI::ProgressThreadLoop() {
  double interval = 1.0 / kAnimationFps;
  while (true) {
    double start = now();
    pthread_mutex_lock(&updateMutex);

    bool redraw = false;

    // update the installation animation, if active
    // skip this if we have a text overlay (too expensive to update)
    if ((currentIcon == INSTALLING_UPDATE || currentIcon == ERASING)) {
      if (!intro_done) {
        if (current_frame == intro_frames - 1) {
          intro_done = true;
          current_frame = 0;
        } else {
          ++current_frame;
        }
      } else {
        current_frame = (current_frame + 1) % loop_frames;
      }

      redraw = true;
    }

    // move the progress bar forward on timed intervals, if configured
    int duration = progressScopeDuration;
    if (progressBarType == DETERMINATE && duration > 0) {
      double elapsed = now() - progressScopeTime;
      float p = 1.0 * elapsed / duration;
      if (p > 1.0) p = 1.0;
      if (p > progress) {
        progress = p;
        redraw = true;
      }
    }

    if (redraw) update_progress_locked();

    pthread_mutex_unlock(&updateMutex);

    if (progressBarType == EMPTY) break;

    double end = now();
    // minimum of 20ms delay between frames
    double delay = interval - (end - start);
    if (delay < 0.02) delay = 0.02;
    usleep(static_cast<useconds_t>(delay * 1000000));
  }
}

void ScreenRecoveryUI::LoadBitmap(const char* filename, GRSurface** surface) {
  int result = res_create_display_surface(filename, surface);
  if (result < 0) {
    LOG(ERROR) << "couldn't load bitmap " << filename << " (error " << result << ")";
  }
}

void ScreenRecoveryUI::FreeBitmap(GRSurface* surface) {
  res_free_surface(surface);
}

void ScreenRecoveryUI::LoadLocalizedBitmap(const char* filename, GRSurface** surface) {
  int result = res_create_localized_alpha_surface(filename, locale_.c_str(), surface);
  if (result < 0) {
    LOG(ERROR) << "couldn't load bitmap " << filename << " (error " << result << ")";
  }
}

static char** Alloc2d(size_t rows, size_t cols) {
  char** result = new char*[rows];
  for (size_t i = 0; i < rows; ++i) {
    result[i] = new char[cols];
    memset(result[i], 0, cols);
  }
  return result;
}

// Choose the right background string to display during update.
void ScreenRecoveryUI::SetSystemUpdateText(bool security_update) {
  if (security_update) {
    LoadLocalizedBitmap("installing_security_text", &installing_text);
  } else {
    LoadLocalizedBitmap("installing_text", &installing_text);
  }
}

bool ScreenRecoveryUI::InitTextParams() {
  if (gr_init() < 0) {
    return false;
  }

  gr_font_size(gr_sys_font(), &char_width_, &char_height_);
  gr_font_size(gr_menu_font(), &menu_char_width_, &menu_char_height_);
  text_rows_ = (ScreenHeight() - kMarginHeight * 2) / char_height_;
  text_cols_ = (ScreenWidth() - kMarginWidth * 2) / char_width_;
  return true;
}

bool ScreenRecoveryUI::Init(const std::string& locale) {
  RecoveryUI::Init(locale);

  if (!InitTextParams()) {
    return false;
  }

#ifdef RECOVERY_UI_BLANK_UNBLANK_ON_INIT
  gr_fb_blank(true);
  gr_fb_blank(false);
#endif

  // Are we portrait or landscape?
  layout_ = (gr_fb_width() > gr_fb_height()) ? LANDSCAPE : PORTRAIT;
  // Are we the large variant of our base layout?
  if (gr_fb_height() > PixelsFromDp(800)) ++layout_;

  text_ = Alloc2d(text_rows_, text_cols_ + 1);
  file_viewer_text_ = Alloc2d(text_rows_, text_cols_ + 1);

  text_row_ = text_rows_ - 1;  // Printed text grows bottom up
  text_col_ = 0;

  // Set up the locale info.
  SetLocale(locale);

  // Load logo and scale it if necessary
  // Note 2/45 is our standard margin on each side so the maximum image
  // width is 41/45 of the screen width.
  GRSurface* image;
  LoadBitmap("logo_image", &image);
  if ((int)gr_get_width(image) > gr_fb_width() * 41 / 45) {
    float scale = (float)gr_fb_width() / (float)gr_get_width(image) * (41.0f / 45.0f);
    GRSurface* scaled_image;
    res_create_scaled_surface(&scaled_image, image, scale, scale);
    logo_image = scaled_image;
    res_free_surface(image);
  } else {
    logo_image = image;
  }

  LoadBitmap("ic_back", &ic_back);
  LoadBitmap("ic_back_sel", &ic_back_sel);

  LoadBitmap("icon_error", &error_icon);

  LoadBitmap("progress_empty", &progressBarEmpty);
  LoadBitmap("progress_fill", &progressBarFill);

  LoadBitmap("stage_empty", &stageMarkerEmpty);
  LoadBitmap("stage_fill", &stageMarkerFill);

  // Background text for "installing_update" could be "installing update"
  // or "installing security update". It will be set after UI init according
  // to commands in BCB.
  installing_text = nullptr;
  LoadLocalizedBitmap("erasing_text", &erasing_text);
  LoadLocalizedBitmap("no_command_text", &no_command_text);
  LoadLocalizedBitmap("error_text", &error_text);

  LoadAnimation();

  return true;
}

void ScreenRecoveryUI::Stop() {
  RecoveryUI::Stop();
  gr_fb_blank(true);
}

void ScreenRecoveryUI::LoadAnimation() {
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/res/images"), closedir);
  dirent* de;
  std::vector<std::string> intro_frame_names;
  std::vector<std::string> loop_frame_names;

  while ((de = readdir(dir.get())) != nullptr) {
    int value, num_chars;
    if (sscanf(de->d_name, "intro%d%n.png", &value, &num_chars) == 1) {
      intro_frame_names.emplace_back(de->d_name, num_chars);
    } else if (sscanf(de->d_name, "loop%d%n.png", &value, &num_chars) == 1) {
      loop_frame_names.emplace_back(de->d_name, num_chars);
    }
  }

  intro_frames = intro_frame_names.size();
  loop_frames = loop_frame_names.size();

  // It's okay to not have an intro.
  if (intro_frames == 0) intro_done = true;
  // But you must have an animation.
  if (loop_frames == 0) abort();

  std::sort(intro_frame_names.begin(), intro_frame_names.end());
  std::sort(loop_frame_names.begin(), loop_frame_names.end());

  introFrames = new GRSurface*[intro_frames];
  for (size_t i = 0; i < intro_frames; i++) {
    LoadBitmap(intro_frame_names.at(i).c_str(), &introFrames[i]);
  }

  loopFrames = new GRSurface*[loop_frames];
  for (size_t i = 0; i < loop_frames; i++) {
    LoadBitmap(loop_frame_names.at(i).c_str(), &loopFrames[i]);
  }
}

void ScreenRecoveryUI::SetBackground(Icon icon) {
  pthread_mutex_lock(&updateMutex);

  if (icon != currentIcon) {
    currentIcon = icon;
  }

  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::SetProgressType(ProgressType type) {
  pthread_mutex_lock(&updateMutex);
  if (progressBarType != type) {
    progressBarType = type;
    progressScopeStart = 0;
    progressScopeSize = 0;
    progress = 0;
    if (progressBarType != EMPTY) {
      update_screen_locked();
      pthread_create(&progress_thread_, nullptr, ProgressThreadStartRoutine, this);
    }
  }
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::ShowProgress(float portion, float seconds) {
  pthread_mutex_lock(&updateMutex);
  progressBarType = DETERMINATE;
  progressScopeStart += progressScopeSize;
  progressScopeSize = portion;
  progressScopeTime = now();
  progressScopeDuration = seconds;
  progress = 0;
  update_progress_locked();
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::SetProgress(float fraction) {
  pthread_mutex_lock(&updateMutex);
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  if (progressBarType == DETERMINATE && fraction > progress) {
    // Skip updates that aren't visibly different.
    int width = gr_get_width(progressBarEmpty);
    float scale = width * progressScopeSize;
    if ((int)(progress * scale) != (int)(fraction * scale)) {
      progress = fraction;
      update_progress_locked();
    }
  }
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::SetStage(int current, int max) {
  pthread_mutex_lock(&updateMutex);
  stage = current;
  max_stage = max;
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::NewLine() {
  // Shift the rows array up
  char* p = text_[0];
  for (size_t i = 0; i < text_rows_ - 1; i++) {
    text_[i] = text_[i + 1];
  }
  text_[text_rows_ - 1] = p;
  memset(text_[text_rows_ - 1], 0, text_cols_ + 1);

  text_col_ = 0;
}

void ScreenRecoveryUI::PrintV(const char* fmt, bool copy_to_stdout, va_list ap) {
  std::string str;
  android::base::StringAppendV(&str, fmt, ap);

  if (copy_to_stdout) {
    fputs(str.c_str(), stdout);
  }

  pthread_mutex_lock(&updateMutex);
  if (text_rows_ > 0 && text_cols_ > 0) {
    if (previous_row_ended) {
      NewLine();
    }
    previous_row_ended = false;

    size_t row = text_rows_ - 1;
    for (const char* ptr = str.c_str(); *ptr != '\0'; ++ptr) {
      if (*ptr == '\n' && *(ptr + 1) == '\0') {
        // Scroll on the next print
        text_[row][text_col_] = '\0';
        previous_row_ended = true;
      } else if ((*ptr == '\n' && *(ptr + 1) != '\0') || text_col_ >= text_cols_) {
        // We need to keep printing, scroll now
        text_[row][text_col_] = '\0';
        NewLine();
      }
      if (*ptr != '\n') text_[row][text_col_++] = *ptr;
    }
    text_[row][text_col_] = '\0';

    if (show_text && update_screen_on_print) {
      update_screen_locked();
    }
  }
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, true, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PrintOnScreenOnly(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, false, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PutChar(char ch) {
  pthread_mutex_lock(&updateMutex);
  if (ch != '\n') text_[text_row_][text_col_++] = ch;
  if (ch == '\n' || text_col_ >= text_cols_) {
    text_col_ = 0;
    ++text_row_;
  }
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::ClearText() {
  pthread_mutex_lock(&updateMutex);
  text_col_ = 0;
  text_row_ = 0;
  for (size_t i = 0; i < text_rows_; ++i) {
    memset(text_[i], 0, text_cols_ + 1);
  }
  pthread_mutex_unlock(&updateMutex);
}

int ScreenRecoveryUI::ShowFile(FILE* fp) {
  std::vector<off_t> offsets;
  offsets.push_back(ftello(fp));
  ClearText();

  struct stat sb;
  fstat(fileno(fp), &sb);

  bool show_prompt = false;
  while (true) {
    if (show_prompt) {
      PrintOnScreenOnly("--(%d%% of %d bytes)--",
                        static_cast<int>(100 * (double(ftello(fp)) / double(sb.st_size))),
                        static_cast<int>(sb.st_size));
      Redraw();
      while (show_prompt) {
        show_prompt = false;
        RecoveryUI::InputEvent evt = WaitInputEvent();
        if (evt.type() != RecoveryUI::EVENT_TYPE_KEY) {
          show_prompt = true;
          continue;
        }
        if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER || evt.key() == KEY_BACKSPACE ||
            evt.key() == KEY_BACK || evt.key() == KEY_HOME || evt.key() == KEY_HOMEPAGE) {
          return evt.key();
        } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP) {
          if (offsets.size() <= 1) {
            show_prompt = true;
          } else {
            offsets.pop_back();
            fseek(fp, offsets.back(), SEEK_SET);
          }
        } else {
          if (feof(fp)) {
            return -1;
          }
          offsets.push_back(ftello(fp));
        }
      }
      ClearText();
    }

    int ch = getc(fp);
    if (ch == EOF) {
      while (text_row_ < text_rows_ - 1) PutChar('\n');
      show_prompt = true;
    } else {
      PutChar(ch);
      if (text_col_ == 0 && text_row_ >= text_rows_ - 1) {
        show_prompt = true;
      }
    }
  }
  return -1;
}

int ScreenRecoveryUI::ShowFile(const char* filename) {
  FILE* fp = fopen_path(filename, "re");
  if (fp == nullptr) {
    Print("  Unable to open %s: %s\n", filename, strerror(errno));
    return -1;
  }

  Icon oldIcon = currentIcon;
  currentIcon = NONE;

  char** old_text = text_;
  size_t old_text_col = text_col_;
  size_t old_text_row = text_row_;

  // Swap in the alternate screen and clear it.
  text_ = file_viewer_text_;
  ClearText();

  int key = ShowFile(fp);
  fclose(fp);

  text_ = old_text;
  text_col_ = old_text_col;
  text_row_ = old_text_row;
  currentIcon = oldIcon;
  return key;
}

void ScreenRecoveryUI::StartMenu(bool is_main, menu_type_t type, const char* const* headers,
                                 const MenuItemVector& items, int initial_selection) {
  pthread_mutex_lock(&updateMutex);
  menu_is_main_ = is_main;
  menu_type_ = type;
  menu_headers_ = headers;
  for (auto& item : items) {
    menu_items_.push_back(ScreenMenuItem(item));
  }
  show_menu = true;
  menu_sel = initial_selection;
  menu_show_start = 0;
  draw_screen_locked();
  if (menu_sel >= menu_show_start + menu_show_count) {
    menu_show_start = menu_sel - (menu_show_count - 1);
  }
  update_screen_locked();
  pthread_mutex_unlock(&updateMutex);
}

int ScreenRecoveryUI::SelectMenu(int sel) {
  pthread_mutex_lock(&updateMutex);
  if (show_menu) {
    int old_menu_sel = menu_sel;

    // Handle wrapping and back item
    if (sel < 0 && (menu_is_main_ || sel < -1)) {
      sel = (int)menu_items_.size() - 1;
    }
    if (sel >= (int)menu_items_.size()) {
      sel = (menu_is_main_ ? 0 : -1);
    }
    menu_sel = sel;

    // Scroll
    if (menu_sel != -1 && sel < menu_show_start) {
      menu_show_start = sel;
    }
    if (sel >= menu_show_start + menu_show_count) {
      menu_show_start = sel - (menu_show_count - 1);
    }

    if (menu_sel != old_menu_sel) update_screen_locked();
  }
  pthread_mutex_unlock(&updateMutex);
  return sel;
}

int ScreenRecoveryUI::SelectMenu(const Point& point) {
  int sel = Device::kNoAction;
  int h_unit = gr_fb_width() / 9;
  int v_unit = gr_fb_height() / 16;
  pthread_mutex_lock(&updateMutex);
  if (show_menu) {
    if (point.y() < menu_start_y_) {
      if (!menu_is_main_ && point.x() >= h_unit / 2 && point.x() < h_unit * 3 / 2 &&
          point.y() >= v_unit * 1 / 2 && point.y() < v_unit * 3 / 2) {
        sel = Device::kGoBack;
      }
    } else {
      int row = -1, col = -1;
      switch (menu_type_) {
        case MT_LIST:
          sel = (point.y() - menu_start_y_) / (menu_char_height_ * 3) + menu_show_start;
          break;
        case MT_GRID:
          row = (point.y() - menu_start_y_) / (gr_fb_height() * 3 / 16);
          col = (point.x()) / (gr_fb_width() / 9);
          if ((col % 4) != 0) {
            sel = row * 2 + ((col - 1) / 4);
          }
          break;
        default:
          break;
      }
      if (sel >= (int)menu_items_.size()) {
        sel = Device::kNoAction;
      }
    }
    if (sel != -1 && sel != menu_sel) {
      menu_sel = sel;
      update_screen_locked();
      usleep(100 * 1000);
    }
  }
  pthread_mutex_unlock(&updateMutex);
  return sel;
}

int ScreenRecoveryUI::ScrollMenu(int updown) {
  pthread_mutex_lock(&updateMutex);
  if ((updown > 0 && menu_show_start + menu_show_count < (int)menu_items_.size()) ||
      (updown < 0 && menu_show_start > 0)) {
    menu_show_start += updown;

    /* We can receive a kInvokeItem event from a different source than touch,
       like from Power button. For this reason, selection should not get out of
       the screen. Constrain it to the first or last visible item of the list */
    if (menu_sel < menu_show_start) {
      menu_sel = menu_show_start;
    } else if (menu_sel >= menu_show_start + menu_show_count) {
      menu_sel = menu_show_start + menu_show_count - 1;
    }

    update_screen_locked();
  }
  pthread_mutex_unlock(&updateMutex);
  return menu_sel;
}

void ScreenRecoveryUI::EndMenu() {
  pthread_mutex_lock(&updateMutex);
  if (show_menu && text_rows_ > 0 && text_cols_ > 0) {
    show_menu = false;
  }
  menu_type_ = MT_NONE;
  menu_headers_ = nullptr;
  menu_items_.clear();
  pthread_mutex_unlock(&updateMutex);
}

bool ScreenRecoveryUI::IsTextVisible() {
  pthread_mutex_lock(&updateMutex);
  int visible = show_text;
  pthread_mutex_unlock(&updateMutex);
  return visible;
}

bool ScreenRecoveryUI::WasTextEverVisible() {
  pthread_mutex_lock(&updateMutex);
  int ever_visible = show_text_ever;
  pthread_mutex_unlock(&updateMutex);
  return ever_visible;
}

void ScreenRecoveryUI::ShowText(bool visible) {
  pthread_mutex_lock(&updateMutex);
  show_text = visible;
  if (show_text) show_text_ever = true;
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::Redraw() {
  pthread_mutex_lock(&updateMutex);
  update_screen_locked();
  pthread_mutex_unlock(&updateMutex);
}

void ScreenRecoveryUI::KeyLongPress(int) {
  // Redraw so that if we're in the menu, the highlight
  // will change color to indicate a successful long press.
  Redraw();
}

void ScreenRecoveryUI::SetLocale(const std::string& new_locale) {
  locale_ = new_locale;
  rtl_locale_ = false;

  if (!new_locale.empty()) {
    size_t underscore = new_locale.find('_');
    // lang has the language prefix prior to '_', or full string if '_' doesn't exist.
    std::string lang = new_locale.substr(0, underscore);

    // A bit cheesy: keep an explicit list of supported RTL languages.
    if (lang == "ar" ||  // Arabic
        lang == "fa" ||  // Persian (Farsi)
        lang == "he" ||  // Hebrew (new language code)
        lang == "iw" ||  // Hebrew (old language code)
        lang == "ur") {  // Urdu
      rtl_locale_ = true;
    }
  }
}
