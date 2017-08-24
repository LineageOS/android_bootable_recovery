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

#include "install/fuse_sdcard_install.h"

#include <dirent.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "bootloader_message/bootloader_message.h"
#include "fuse_provider.h"
#include "fuse_sideload.h"
#include "install/install.h"
#include "otautil/roots.h"

static constexpr const char* SDCARD_ROOT = "/sdcard";

// Set the BCB to reboot back into recovery (it won't resume the install from
// sdcard though).
static void SetSdcardUpdateBootloaderMessage() {
  std::vector<std::string> options;
  std::string err;
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
}

// Returns the selected filename, or an empty string.
static std::string BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui) {
  ensure_path_mounted(path);

  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
  if (!d) {
    PLOG(ERROR) << "error opening " << path;
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> entries{ "../" };  // "../" is always the first entry.

  dirent* de;
  while ((de = readdir(d.get())) != nullptr) {
    std::string name(de->d_name);

    if (de->d_type == DT_DIR) {
      // Skip "." and ".." entries.
      if (name == "." || name == "..") continue;
      dirs.push_back(name + "/");
    } else if (de->d_type == DT_REG && android::base::EndsWithIgnoreCase(name, ".zip")) {
      entries.push_back(name);
    }
  }

  std::sort(dirs.begin(), dirs.end());
  std::sort(entries.begin(), entries.end());

  // Append dirs to the entries list.
  entries.insert(entries.end(), dirs.begin(), dirs.end());

  std::vector<std::string> headers{ "Choose a package to install:", path };

  size_t chosen_item = 0;
  while (true) {
    chosen_item = ui->ShowMenu(
        headers, entries, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    // Return if WaitKey() was interrupted.
    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
      return "";
    }
    if (chosen_item == Device::kGoHome) {
      return "@";
    }
    if (chosen_item == Device::kGoBack || chosen_item == 0) {
      // Go up but continue browsing (if the caller is browse_directory).
      return "";
    }

    const std::string& item = entries[chosen_item];

    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      // Recurse down into a subdirectory.
      new_path.pop_back();
      std::string result = BrowseDirectory(new_path, device, ui);
      if (!result.empty()) return result;
    } else {
      // Selected a zip file: return the path to the caller.
      return new_path;
    }
  }

  // Unreachable.
}

struct token {
  pthread_t th;
  const char* path;
  int result;
};

static void* run_sdcard_fuse(void* cookie) {
  token* t = reinterpret_cast<token*>(cookie);

  struct stat sb;
  if (stat(t->path, &sb) < 0) {
    fprintf(stderr, "failed to stat %s: %s\n", t->path, strerror(errno));
    t->result = -1;
    return nullptr;
  }

  auto file_data_reader = std::make_unique<FuseFileDataProvider>(t->path, 65536);
  if (file_data_reader->Valid()) {
    fprintf(stderr, "failed to open %s: %s\n", t->path, strerror(errno));
    t->result = -1;
    return nullptr;
  }

  t->result = run_fuse_sideload(std::move(file_data_reader));
  return nullptr;
}

// How long (in seconds) we wait for the fuse-provided package file to
// appear, before timing out.
#define SDCARD_INSTALL_TIMEOUT 10

static void* StartSdcardFuse(const std::string& path) {
  token* t = new token;

  t->path = path.c_str();
  pthread_create(&(t->th), NULL, run_sdcard_fuse, t);

  struct stat st;
  int i;
  for (i = 0; i < SDCARD_INSTALL_TIMEOUT; ++i) {
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &st) != 0) {
      if (errno == ENOENT && i < SDCARD_INSTALL_TIMEOUT - 1) {
        sleep(1);
        continue;
      } else {
        return nullptr;
      }
    }
  }

  // The installation process expects to find the sdcard unmounted. Unmount it with MNT_DETACH so
  // that our open file continues to work but new references see it as unmounted.
  umount2("/sdcard", MNT_DETACH);

  return t;
}

static void FinishSdcardFuse(void* cookie) {
  if (cookie == NULL) return;
  token* t = reinterpret_cast<token*>(cookie);

  // Calling stat() on this magic filename signals the fuse
  // filesystem to shut down.
  struct stat st;
  stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &st);

  pthread_join(t->th, nullptr);
  delete t;
}

int ApplyFromSdcard(Device* device, RecoveryUI* ui) {
  if (ensure_path_mounted(SDCARD_ROOT) != 0) {
    LOG(ERROR) << "\n-- Couldn't mount " << SDCARD_ROOT << ".\n";
    return INSTALL_ERROR;
  }

  std::string path = BrowseDirectory(SDCARD_ROOT, device, ui);
  if (path == "@") {
    return INSTALL_NONE;
  }
  if (path.empty()) {
    LOG(ERROR) << "\n-- No package file selected.\n";
    ensure_path_unmounted(SDCARD_ROOT);
    return INSTALL_ERROR;
  }

  ui->Print("\n-- Install %s ...\n", path.c_str());
  SetSdcardUpdateBootloaderMessage();
  void* token = StartSdcardFuse(path);
  int status = install_package(FUSE_SIDELOAD_HOST_PATHNAME, false, false, 0 /*retry_count*/, ui);
  FinishSdcardFuse(token);

  ensure_path_unmounted(SDCARD_ROOT);
  return status;
}
