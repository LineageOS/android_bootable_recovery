/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "fastboot.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <bootloader_message/bootloader_message.h>

#include "recovery_ui/ui.h"

static const std::vector<std::pair<std::string, Device::BuiltinAction>> kFastbootMenuActions{
  { "Reboot system now", Device::REBOOT_FROM_FASTBOOT },
  { "Enter recovery", Device::ENTER_RECOVERY },
  { "Reboot to bootloader", Device::REBOOT_BOOTLOADER },
  { "Power off", Device::SHUTDOWN_FROM_FASTBOOT },
};

void FillDefaultFastbootLines(std::vector<std::string>& title_lines) {
  std::string bootloader_version = android::base::GetProperty("ro.bootloader", "");
  std::string baseband_version = android::base::GetProperty("ro.build.expect.baseband", "");
  std::string hw_version = android::base::GetProperty("ro.revision", "");
  title_lines.push_back("Product name - " + android::base::GetProperty("ro.product.device", ""));
  if (!android::base::EqualsIgnoreCase(bootloader_version, "unknown")) {
    title_lines.push_back("Bootloader version - " + bootloader_version);
  }
  if (!baseband_version.empty()) {
    title_lines.push_back("Baseband version - " + baseband_version);
  }
  title_lines.push_back("Serial number - " + android::base::GetProperty("ro.serialno", ""));
  title_lines.push_back(std::string("Secure boot - ") +
                        ((android::base::GetProperty("ro.secure", "") == "1") ? "yes" : "no"));
  if (!android::base::EqualsIgnoreCase(hw_version, "0")) {
    title_lines.push_back("HW version - " + hw_version);
  }
}

void FillWearableFastbootLines(std::vector<std::string>& title_lines) {
  title_lines.push_back("Android Fastboot");
  title_lines.push_back(android::base::GetProperty("ro.product.device", "") + " - " +
                        android::base::GetProperty("ro.revision", ""));
  title_lines.push_back(android::base::GetProperty("ro.bootloader", ""));

  const size_t max_baseband_len = 24;
  const std::string& baseband = android::base::GetProperty("ro.build.expect.baseband", "");
  title_lines.push_back(baseband.length() > max_baseband_len
                            ? baseband.substr(0, max_baseband_len - 3) + "..."
                            : baseband);

  title_lines.push_back("Serial #: " + android::base::GetProperty("ro.serialno", ""));
}

Device::BuiltinAction StartFastboot(Device* device, const std::vector<std::string>& /* args */) {
  RecoveryUI* ui = device->GetUI();
  std::vector<std::string> title_lines;

  if (ui->IsWearable()) {
    FillWearableFastbootLines(title_lines);
  } else {
    ui->SetEnableFastbootdLogo(true);
    FillDefaultFastbootLines(title_lines);
  }

  ui->ResetKeyInterruptStatus();
  ui->SetTitle(title_lines);
  ui->ShowText(true);
  device->StartFastboot();

  // Reset to normal system boot so recovery won't cycle indefinitely.
  // TODO(b/112277594) Clear only if 'recovery' field of BCB is empty. If not,
  // set the 'command' field of BCB to 'boot-recovery' so the next boot is into recovery
  // to finish any interrupted tasks.
  std::string err;
  if (!clear_bootloader_message(&err)) {
    LOG(ERROR) << "Failed to clear BCB message: " << err;
  }

  std::vector<std::string> fastboot_menu_items;
  std::transform(kFastbootMenuActions.cbegin(), kFastbootMenuActions.cend(),
                 std::back_inserter(fastboot_menu_items),
                 [](const auto& entry) { return entry.first; });

  auto chosen_item = ui->ShowMenu(
      {}, fastboot_menu_items, 0, false,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

  if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
    return Device::KEY_INTERRUPTED;
  }
  if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::TIMED_OUT)) {
    return Device::BuiltinAction::NO_ACTION;
  }
  return kFastbootMenuActions[chosen_item].second;
}
