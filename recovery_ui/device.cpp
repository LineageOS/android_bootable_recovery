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

#include "recovery_ui/device.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <android-base/logging.h>

#include "otautil/boot_state.h"
#include "recovery_ui/ui.h"

typedef std::pair<std::string, Device::BuiltinAction> menu_action_t;

static std::vector<std::string> g_main_header{};
static std::vector<menu_action_t> g_main_actions{
  { "Reboot system now", Device::REBOOT },
  { "Apply update", Device::APPLY_UPDATE },
  { "Factory reset", Device::MENU_WIPE },
  { "Advanced", Device::MENU_ADVANCED },
};

static std::vector<std::string> g_advanced_header{ "Advanced options" };
static std::vector<menu_action_t> g_advanced_actions{
  { "Enter fastboot", Device::ENTER_FASTBOOT },
  { "Reboot to bootloader", Device::REBOOT_BOOTLOADER },
  { "Reboot to recovery", Device::REBOOT_RECOVERY },
  { "Mount/unmount system", Device::MOUNT_SYSTEM },
  { "View recovery logs", Device::VIEW_RECOVERY_LOGS },
  { "Enable ADB", Device::ENABLE_ADB },
  { "Switch slot", Device::SWAP_SLOT },
  { "Run graphics test", Device::RUN_GRAPHICS_TEST },
  { "Run locale test", Device::RUN_LOCALE_TEST },
  { "Enter rescue", Device::ENTER_RESCUE },
  { "Power off", Device::SHUTDOWN },
};

static std::vector<std::string> g_wipe_header{ "Factory reset" };
static std::vector<menu_action_t> g_wipe_actions{
  { "Format data/factory reset", Device::WIPE_DATA },
  { "Format cache partition", Device::WIPE_CACHE },
  { "Format system partition", Device::WIPE_SYSTEM },
};

static std::vector<menu_action_t>* current_menu_ = &g_main_actions;
static std::vector<std::string> g_menu_items;

static void PopulateMenuItems() {
  g_menu_items.clear();
  std::transform(current_menu_->cbegin(), current_menu_->cend(), std::back_inserter(g_menu_items),
                 [](const auto& entry) { return entry.first; });
}

Device::Device(RecoveryUI* ui) : ui_(ui) {
  ui->SetDevice(this);
  PopulateMenuItems();
}

void Device::GoHome() {
  current_menu_ = &g_main_actions;
  PopulateMenuItems();
}

static void RemoveMenuItemForAction(std::vector<menu_action_t>& menu, Device::BuiltinAction action) {
  menu.erase(
      std::remove_if(menu.begin(), menu.end(),
                     [action](const auto& entry) { return entry.second == action; }), menu.end());
  CHECK(!menu.empty());
}

void Device::RemoveMenuItemForAction(Device::BuiltinAction action) {
  ::RemoveMenuItemForAction(g_wipe_actions, action);
  ::RemoveMenuItemForAction(g_advanced_actions, action);
}

const std::vector<std::string>& Device::GetMenuItems() {
  return g_menu_items;
}

const std::vector<std::string>& Device::GetMenuHeaders() {
  if (current_menu_ == &g_wipe_actions)
      return g_wipe_header;
  if (current_menu_ == &g_advanced_actions)
      return g_advanced_header;
  return g_main_header;
}

Device::BuiltinAction Device::InvokeMenuItem(size_t menu_position) {
  Device::BuiltinAction action = (*current_menu_)[menu_position].second;

  if (action > MENU_BASE) {
    switch (action) {
      case Device::BuiltinAction::MENU_WIPE:
        current_menu_ = &g_wipe_actions;
        break;
      case Device::BuiltinAction::MENU_ADVANCED:
        current_menu_ = &g_advanced_actions;
        break;
      default:
        break;
    }
    PopulateMenuItems();
  }
  return action;
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
    case BTN_NORTH:
    case BTN_DPAD_DOWN:
      return kHighlightDown;

    case KEY_UP:
    case KEY_VOLUMEUP:
    case KEY_SEARCH:
    case BTN_WEST:
    case BTN_DPAD_UP:
      return kHighlightUp;

    case KEY_SCROLLUP:
      return kScrollUp;
    case KEY_SCROLLDOWN:
      return kScrollDown;

    case KEY_ENTER:
    case KEY_POWER:
    case BTN_MOUSE:
    case KEY_SEND:
    case BTN_SOUTH:
    case BTN_START:
      return kInvokeItem;

    case KEY_HOME:
    case KEY_HOMEPAGE:
      return kGoHome;

    case KEY_BACKSPACE:
    case KEY_BACK:
      return kGoBack;

    case KEY_AGAIN:
      return kDoSideload;

    case KEY_REFRESH:
      return kRefresh;

    default:
      // If you have all of the above buttons, any other buttons
      // are ignored. Otherwise, any button cycles the highlight.
      return ui_->HasThreeButtons() ? kNoAction : kHighlightDown;
  }
}

void Device::SetBootState(const BootState* state) {
  boot_state_ = state;
}

std::optional<std::string> Device::GetReason() const {
  return boot_state_ ? std::make_optional(boot_state_->reason()) : std::nullopt;
}

std::optional<std::string> Device::GetStage() const {
  return boot_state_ ? std::make_optional(boot_state_->stage()) : std::nullopt;
}
