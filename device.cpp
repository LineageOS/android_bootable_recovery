/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "device.h"

#define ARRAY_SIZE(A) (sizeof(A) / sizeof(*(A)))

// *** Main menu ***
static const menu_type_t MAIN_MENU_TYPE = MT_GRID;
static const MenuItem MAIN_MENU_ITEMS[] = {
  MenuItem("Reboot", "ic_reboot", "ic_reboot_sel"),
  MenuItem("Apply update", "ic_system_update", "ic_system_update_sel"),
  MenuItem("Factory reset", "ic_factory_reset", "ic_factory_reset_sel"),
  MenuItem("Advanced", "ic_options_advanced", "ic_options_advanced_sel"),
};
static const MenuItemVector main_menu_items_ =
    MenuItemVector(MAIN_MENU_ITEMS, MAIN_MENU_ITEMS + ARRAY_SIZE(MAIN_MENU_ITEMS));
static const Device::BuiltinAction MAIN_MENU_ACTIONS[] = {
  Device::REBOOT,
  Device::APPLY_UPDATE,
  Device::WIPE_MENU,
  Device::ADVANCED_MENU,
};
static const Device::MenuActionVector main_menu_actions_ =
    Device::MenuActionVector(MAIN_MENU_ACTIONS, MAIN_MENU_ACTIONS + ARRAY_SIZE(MAIN_MENU_ACTIONS));
static_assert(ARRAY_SIZE(MAIN_MENU_ITEMS) == ARRAY_SIZE(MAIN_MENU_ACTIONS),
              "MAIN_MENU_ITEMS and MAIN_MENU_ACTIONS should have the same length.");

// *** Wipe menu ***
static const menu_type_t WIPE_MENU_TYPE = MT_LIST;
static const MenuItem WIPE_MENU_ITEMS[] = {
  MenuItem("Wipe data / factory reset"),
#ifndef AB_OTA_UPDATER
  MenuItem("Wipe cache"),
#endif
  MenuItem("Wipe system"),
};
static const MenuItemVector wipe_menu_items_ =
    MenuItemVector(WIPE_MENU_ITEMS, WIPE_MENU_ITEMS + ARRAY_SIZE(WIPE_MENU_ITEMS));
static const Device::BuiltinAction WIPE_MENU_ACTIONS[] = {
  Device::WIPE_DATA,
#ifndef AB_OTA_UPDATER
  Device::WIPE_CACHE,
#endif
  Device::WIPE_SYSTEM,
};
static const Device::MenuActionVector wipe_menu_actions_ =
    Device::MenuActionVector(WIPE_MENU_ACTIONS, WIPE_MENU_ACTIONS + ARRAY_SIZE(WIPE_MENU_ACTIONS));
static_assert(ARRAY_SIZE(WIPE_MENU_ITEMS) == ARRAY_SIZE(WIPE_MENU_ACTIONS),
              "WIPE_MENU_ITEMS and WIPE_MENU_ACTIONS should have the same length.");

// *** Advanced menu
static const menu_type_t ADVANCED_MENU_TYPE = MT_LIST;

static const MenuItem ADVANCED_MENU_ITEMS[] = {
#ifdef DOWNLOAD_MODE
  MenuItem("Reboot to download mode"),
#else
  MenuItem("Reboot to bootloader"),
#endif
  MenuItem("Reboot to recovery"),
  MenuItem("Mount system"),
  MenuItem("View logs"),
  MenuItem("Run graphics test"),
  MenuItem("Run locale test"),
  MenuItem("Power off"),
};
static const MenuItemVector advanced_menu_items_ =
    MenuItemVector(ADVANCED_MENU_ITEMS, ADVANCED_MENU_ITEMS + ARRAY_SIZE(ADVANCED_MENU_ITEMS));

static const Device::BuiltinAction ADVANCED_MENU_ACTIONS[] = {
  Device::REBOOT_BOOTLOADER,
  Device::REBOOT_RECOVERY,
  Device::MOUNT_SYSTEM,
  Device::VIEW_RECOVERY_LOGS,
  Device::RUN_GRAPHICS_TEST,
  Device::RUN_LOCALE_TEST,
  Device::SHUTDOWN,
};
static const Device::MenuActionVector advanced_menu_actions_ = Device::MenuActionVector(
    ADVANCED_MENU_ACTIONS, ADVANCED_MENU_ACTIONS + ARRAY_SIZE(ADVANCED_MENU_ACTIONS));

static_assert(ARRAY_SIZE(ADVANCED_MENU_ITEMS) == ARRAY_SIZE(ADVANCED_MENU_ACTIONS),
              "ADVANCED_MENU_ITEMS and ADVANCED_MENU_ACTIONS should have the same length.");

Device::Device(RecoveryUI* ui) : ui_(ui) {
  GoHome();
}

Device::BuiltinAction Device::InvokeMenuItem(int menu_position) {
  if (menu_position < 0) {
    if (menu_position == Device::kGoBack || menu_position == Device::kGoHome) {
      // Assume only two menu levels, so back is equivalent to home.
      GoHome();
    }
    return NO_ACTION;
  }
  BuiltinAction action = menu_actions_.at(menu_position);
  switch (action) {
    case WIPE_MENU:
      menu_is_main_ = false;
      menu_type_ = WIPE_MENU_TYPE;
      menu_items_ = wipe_menu_items_;
      menu_actions_ = wipe_menu_actions_;
      break;
    case ADVANCED_MENU:
      menu_is_main_ = false;
      menu_type_ = ADVANCED_MENU_TYPE;
      menu_items_ = advanced_menu_items_;
      menu_actions_ = advanced_menu_actions_;
      break;
    default:
      break;  // Fall through
  }
  return action;
}

void Device::GoHome() {
  menu_is_main_ = true;
  menu_type_ = MAIN_MENU_TYPE;
  menu_items_ = main_menu_items_;
  menu_actions_ = main_menu_actions_;
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
