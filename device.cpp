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

static const menu_type MAIN_MENU_TYPE = GRID;

static const menu_item MAIN_MENU_ITEMS[] = {
  { "Reboot", "ic_reboot" },
  { "Apply update", "ic_system_update" },
  { "Factory reset", "ic_factory_reset" },
  { "Advanced", "ic_options_advanced" },
  { nullptr, nullptr }
};

static const Device::BuiltinAction MAIN_MENU_ACTIONS[] = {
  Device::REBOOT,
  Device::APPLY_UPDATE,
  Device::WIPE_DATA,
  Device::ADVANCED_MENU,
};

static_assert(sizeof(MAIN_MENU_ITEMS) / sizeof(MAIN_MENU_ITEMS[0]) ==
              sizeof(MAIN_MENU_ACTIONS) / sizeof(MAIN_MENU_ACTIONS[0]) + 1,
              "MAIN_MENU_ITEMS and MAIN_MENU_ACTIONS should have the same length, "
              "except for the extra NULL entry in MAIN_MENU_ITEMS.");

static const menu_type ADVANCED_MENU_TYPE = LIST;

static const menu_item ADVANCED_MENU_ITEMS[] = {
  { "Reboot to bootloader", nullptr },
  { "Mount /system", nullptr },
#ifndef AB_OTA_UPDATER
  { "Wipe /cache", nullptr },
#endif
  { "View logs", nullptr },
  { "Run graphics test", nullptr },
  { "Power off", nullptr },
  { nullptr, nullptr }
};

static const Device::BuiltinAction ADVANCED_MENU_ACTIONS[] = {
  Device::REBOOT_BOOTLOADER,
  Device::MOUNT_SYSTEM,
#ifndef AB_OTA_UPDATER
  Device::WIPE_CACHE,
#endif
  Device::VIEW_RECOVERY_LOGS,
  Device::RUN_GRAPHICS_TEST,
  Device::SHUTDOWN,
};

static_assert(sizeof(ADVANCED_MENU_ITEMS) / sizeof(ADVANCED_MENU_ITEMS[0]) ==
              sizeof(ADVANCED_MENU_ACTIONS) / sizeof(ADVANCED_MENU_ACTIONS[0]) + 1,
              "ADVANCED_MENU_ITEMS and ADVANCED_MENU_ACTIONS should have the same length, "
              "except for the extra NULL entry in ADVANCED_MENU_ITEMS.");

Device::Device(RecoveryUI* ui) :
  ui_(ui)
{
  menu_.type = MAIN_MENU_TYPE;
  menu_.items = MAIN_MENU_ITEMS;
  menu_actions_ = MAIN_MENU_ACTIONS;
}

const menu& Device::GetMenu() {
  return menu_;
}

Device::BuiltinAction Device::InvokeMenuItem(int menu_position) {
  if (menu_position < 0) {
    if (menu_position == Device::kGoBack ||
        menu_position == Device::kGoHome) {
      menu_.type = MAIN_MENU_TYPE;
      menu_.items = MAIN_MENU_ITEMS;
      menu_actions_ = MAIN_MENU_ACTIONS;
    }
    return NO_ACTION;
  }
  Device::BuiltinAction action = menu_actions_[menu_position];
  if (action == Device::ADVANCED_MENU) {
    menu_.type = ADVANCED_MENU_TYPE;
    menu_.items = ADVANCED_MENU_ITEMS;
    menu_actions_ = ADVANCED_MENU_ACTIONS;
  }
  return action;
}

int Device::HandleMenuKey(int key, bool visible) {
  if (!visible) {
    return kNoAction;
  }

  switch (key) {
    case KEY_DOWN:
    case KEY_VOLUMEDOWN:
      return kHighlightDown;

    case KEY_UP:
    case KEY_VOLUMEUP:
      return kHighlightUp;

    case KEY_ENTER:
    case KEY_POWER:
      return kInvokeItem;

    case KEY_BACKSPACE:
    case KEY_BACK:
      return kGoBack;

    case KEY_HOME:
    case KEY_HOMEPAGE:
      return kGoHome;

    default:
      // If you have all of the above buttons, any other buttons
      // are ignored. Otherwise, any button cycles the highlight.
      return ui_->HasThreeButtons() ? kNoAction : kHighlightDown;
  }
}
