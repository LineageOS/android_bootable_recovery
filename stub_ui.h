/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef RECOVERY_STUB_UI_H
#define RECOVERY_STUB_UI_H

#include "ui.h"

// Stub implementation of RecoveryUI for devices without screen.
class StubRecoveryUI : public RecoveryUI {
 public:
  StubRecoveryUI() = default;

  void SetBackground(Icon /* icon */) override {}
  void SetSystemUpdateText(bool /* security_update */) override {}

  // progress indicator
  void SetProgressType(ProgressType /* type */) override {}
  void ShowProgress(float /* portion */, float /* seconds */) override {}
  void SetProgress(float /* fraction */) override {}

  void SetStage(int /* current */, int /* max */) override {}

  // text log
  void ShowText(bool /* visible */) override {}
  bool IsTextVisible() override {
    return false;
  }
  bool WasTextEverVisible() override {
    return false;
  }
  void UpdateScreenOnPrint(bool /* update */) override{};

  // printing messages
  void Print(const char* fmt, ...) override {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
  }
  void PrintOnScreenOnly(const char* /* fmt */, ...) override {}
  int ShowFile(const char* /* filename */) override {
    return -1;
  }
  void Redraw() override {}

  // menu display
  void StartMenu(bool /* is_main */, menu_type_t /* type */, const char* const* /* headers */,
                 const MenuItemVector& /* items */, int /* initial_selection */) override {}
  int SelectMenu(int sel) override {
    return sel;
  }
  int SelectMenu(const Point& /* point */) override {
    return 0;
  }
  int ScrollMenu(int /* updown */) override {
    return 0;
  }
  void EndMenu() override {}

  bool MenuShowing() const {
    return true;
  }
  bool MenuScrollable() const {
    return true;
  }
  int MenuItemStart() const {
    return 0;
  }
  int MenuItemHeight() const {
    return 1;
  }
};

#endif  // RECOVERY_STUB_UI_H
