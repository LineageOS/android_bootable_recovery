/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "device.h"

static const char* MENU_ITEMS[] = {
  "Reboot system now",
  "Reboot to bootloader",
  "Apply update",
  "Wipe data/factory reset",
#ifndef AB_OTA_UPDATER
  "Wipe cache partition",
#endif  // !AB_OTA_UPDATER
  "Wipe system partition",
  "Mount /system",
  "View recovery logs",
  "Run graphics test",
  "Run locale test",
  "Power off",
  nullptr,
};

static const Device::BuiltinAction MENU_ACTIONS[] = {
  Device::REBOOT,
  Device::REBOOT_BOOTLOADER,
  Device::APPLY_UPDATE,
  Device::WIPE_DATA,
#ifndef AB_OTA_UPDATER
  Device::WIPE_CACHE,
#endif  // !AB_OTA_UPDATER
  Device::WIPE_SYSTEM,
  Device::MOUNT_SYSTEM,
  Device::VIEW_RECOVERY_LOGS,
  Device::RUN_GRAPHICS_TEST,
  Device::RUN_LOCALE_TEST,
  Device::SHUTDOWN,
};

static_assert(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]) ==
              sizeof(MENU_ACTIONS) / sizeof(MENU_ACTIONS[0]) + 1,
              "MENU_ITEMS and MENU_ACTIONS should have the same length, "
              "except for the extra NULL entry in MENU_ITEMS.");

const char* const* Device::GetMenuItems() {
  return MENU_ITEMS;
}

Device::BuiltinAction Device::InvokeMenuItem(int menu_position) {
  return menu_position < 0 ? NO_ACTION : MENU_ACTIONS[menu_position];
}

int Device::HandleMenuKey(int key, bool visible) {
  if (!visible) {
    return kNoAction;
  }

  switch (key) {
    case KEY_RIGHTSHIFT:
    case KEY_DOWN:
    case KEY_VOLUMEDOWN:
    case KEY_MENU:
      return kHighlightDown;

    case KEY_LEFTSHIFT:
    case KEY_UP:
    case KEY_VOLUMEUP:
    case KEY_SEARCH:
      return kHighlightUp;

    case KEY_ENTER:
    case KEY_POWER:
    case BTN_MOUSE:
    case KEY_SEND:
      return kInvokeItem;

    case KEY_HOME:
    case KEY_HOMEPAGE:
      return kGoHome;

    case KEY_BACKSPACE:
    case KEY_BACK:
      return kGoBack;

    case KEY_REFRESH:
      return kRefresh;

    default:
      // If you have all of the above buttons, any other buttons
      // are ignored. Otherwise, any button cycles the highlight.
      return ui_->HasThreeButtons() ? kNoAction : kHighlightDown;
  }
}

void
Device::handleEvent(int code, const std::vector<std::string>& argv)
{
    // We really don't care about the actual notification, as we just want
    // to notify the UI that something happened.
    (void)code;
    (void)argv;
    ui_->onVolumeChanged();
}
