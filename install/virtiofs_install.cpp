/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "install/fuse_install.h"
#include "install/virtiofs_install.h"

#include <string>

#include "install/install.h"
#include "recovery_utils/roots.h"

static constexpr const char* VIRTIOFS_MOUNTPOINT = "/mnt/vendor/shared";

InstallResult ApplyFromVirtiofs(Device* device) {
  auto ui = device->GetUI();

  std::string path = BrowseDirectory(VIRTIOFS_MOUNTPOINT, device, ui);
  if (path.empty()) {
    return INSTALL_NONE;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());

  auto package =
      Package::CreateFilePackage(path, std::bind(&RecoveryUI::SetProgress, ui,
                                                 std::placeholders::_1));
  if (package == nullptr) {
    ui->Print("Failed to open package %s\n", path.c_str());
    return INSTALL_ERROR;
  }

  auto result = InstallPackage(package.get(), path, false, 0 /* retry_count */,
                               device);
  return result;
}

bool InitializeVirtiofs() {
  if (volume_for_mount_point(VIRTIOFS_MOUNTPOINT)) {
    if (ensure_path_mounted(VIRTIOFS_MOUNTPOINT) == 0) {
      return true;
    }
  }
  return false;
}
